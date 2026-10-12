// Include the actual service implementation so its private token scope is
// tested without adding a production-only test API. No real clipboard is used.
#include "src/clipboard_files.cpp"
#include <Aclapi.h>

#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using namespace clipboard_files;

namespace {
thread_local unsigned contextDepth = 0;
std::atomic<unsigned> entered {0}, exited {0}, publications {0};

struct probe_context_t {
    impersonation_t identity;
    explicit probe_context_t(HANDLE token): identity(token) { contextDepth++; entered++; }
    ~probe_context_t() { contextDepth--; exited++; }
};

void permissions(const fs::path& path, PSID user, PSID restricted, bool inherit)
{
    EXPLICIT_ACCESSW entries[2] {};
    for (auto& entry : entries) {
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = inherit ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    }
    entries[0].Trustee.ptstrName = static_cast<LPWSTR>(user);
    entries[1].Trustee.ptstrName = static_cast<LPWSTR>(restricted);
    PACL acl = nullptr;
    require(SetEntriesInAclW(restricted ? 2 : 1, entries, nullptr, &acl) == ERROR_SUCCESS, "Cannot build isolated fixture ACL");
    const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    require(result == ERROR_SUCCESS, "Cannot apply isolated fixture ACL");
}

response_t poll(service_t& service, const guard_t& guard, const std::string& id)
{
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        auto response = service.request("context-fixture#1", guard, "status", id, 0, UINT32_MAX, {});
        if (response.status != 202) return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Context fixture timed out");
}

void write(const fs::path& path)
{
    std::ofstream fixture(path, std::ios::binary);
    fixture << "isolated security context fixture";
    require(bool(fixture), "Cannot create context fixture");
}
} // namespace

int main()
{
    const auto parent = fs::absolute(".tools");
    const auto root = parent / fs::path(wide("clipboard-files-context-" + random_id()));
    fs::create_directories(root);
    try {
        HANDLE rawProcess = INVALID_HANDLE_VALUE;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &rawProcess), "Cannot capture fixture process identity");
        clipboard_files::handle_t process(rawProcess);
        DWORD bytes = 0;
        GetTokenInformation(process.value, TokenUser, nullptr, 0, &bytes);
        std::vector<unsigned char> userBuffer(bytes);
        require(GetTokenInformation(process.value, TokenUser, userBuffer.data(), bytes, &bytes), "Cannot inspect fixture identity");
        auto user = reinterpret_cast<TOKEN_USER*>(userBuffer.data())->User.Sid;
        unsigned char restrictedBuffer[SECURITY_MAX_SID_SIZE];
        DWORD restrictedBytes = sizeof(restrictedBuffer);
        // This restricting SID already has read/traverse rights on the workspace
        // ancestors; only this isolated root receives fixture write rights.
        // A protected file with a user-only ACL still fails the restricting-SID
        // access check, even though the ordinary user can read it.
        require(CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, restrictedBuffer, &restrictedBytes), "Cannot create fixture restricted SID");
        SID_AND_ATTRIBUTES restriction {restrictedBuffer, 0};
        HANDLE rawRestricted = INVALID_HANDLE_VALUE;
        require(CreateRestrictedToken(process.value, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restriction, &rawRestricted),
            "Cannot create restricted fixture token");
        auto restricted = std::make_shared<clipboard_files::handle_t>(rawRestricted);
        permissions(root, user, restrictedBuffer, true);
        auto denied = root / L"denied-source.txt";
        write(denied);
        permissions(denied, user, nullptr, false);
        { auto allowed = open_read(denied); file_info(allowed.value); }
        {
            impersonation_t scope(restricted->value);
            clipboard_files::handle_t blocked(CreateFileW(denied.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
            require(blocked.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
                "Restricted fixture token unexpectedly read the denied file");
        }
        { auto restored = open_read(denied); file_info(restored.value); }
        std::cout << "PASS: native token scope denies protected file and restores caller\n";

        std::atomic<uint32_t> sequence {50};
        desktop_t desktop;
        desktop.enter_context = [restricted] { return std::make_shared<probe_context_t>(restricted->value); };
        desktop.sequence = [&] { require(contextDepth > 0, "Sequence callback escaped its user scope"); return sequence.load(); };
        desktop.read = [&] { require(contextDepth > 0, "Read callback escaped its user scope"); return clipboard_snapshot_t {sequence.load(), {denied}}; };
        desktop.publish = [&](const std::vector<fs::path>&, uint32_t expected) {
            require(contextDepth > 0 && sequence == expected, "Publish callback escaped its user scope or sequence");
            publications++;
            return ++sequence;
        };
        guard_t guard = [](const std::function<void()>& action) { action(); return true; };
        {
            service_t service(root / L"denied-stage", desktop);
            response_t offer;
            for (unsigned attempt = 0; attempt < 1000; attempt++) {
                offer = service.request("context-fixture#1", guard, "offer", {}, 0, UINT32_MAX, {});
                if (offer.status != 202) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(offer.status == 409 && offer.body.empty() && publications == 0,
                "Source snapshot bypassed the effective user's denied-read permission");
        }
        require(entered == exited && contextDepth == 0, "Source task retained an impersonation scope");
        std::cout << "PASS: actual asynchronous source cannot bypass effective user permissions\n";

        const auto sourceDirectory = root / L"permitted-sources";
        fs::create_directory(sourceDirectory);
        std::vector<fs::path> roots;
        for (unsigned i = 0; i < 80; i++) {
            auto path = sourceDirectory / fs::path(wide("tiny" + std::to_string(i)));
            write(path);
            roots.push_back(path);
        }
        source_t source(roots, 1);
        desktop.read = [&] { require(contextDepth > 0, "Read callback escaped its user scope"); return clipboard_snapshot_t {sequence.load(), {}}; };
        const auto storage = root / L"upload-stage";
        {
            service_t service(storage, desktop);
            auto begin = service.request("context-fixture#1", guard, "begin", {}, 0, sequence.load(), encode_manifest(source.manifest));
            require(begin.status == 202 && poll(service, guard, begin.id).status == 200, "Restricted stage preparation failed");
            for (uint32_t batch = 0; batch < source.plan.size(); batch++) {
                require(service.request("context-fixture#1", guard, "batch", begin.id, batch, UINT32_MAX, source.read_batch(batch)).status == 200,
                    "Restricted batch staging failed");
            }
            const auto beforeVerify = entered.load();
            require(service.request("context-fixture#1", guard, "commit", begin.id, 0, begin.sequence, {}).status == 202,
                "Restricted verification did not start");
            require(poll(service, guard, begin.id).status == 200 && publications == 1, "Restricted verification or publication failed");
            require(entered >= beforeVerify + parallel_workers + 2, "Parallel verification lanes did not enter separate security scopes");
        }
        require(entered == exited && contextDepth == 0, "A request, verification worker or shutdown retained impersonation");
        std::cout << "PASS: request, four verification lanes and publication use/restores effective user scopes\n";

        desktop.enter_context = []() -> std::shared_ptr<void> { throw std::runtime_error("Fixture context denied"); };
        {
            service_t service(root / L"failed-context", desktop);
            require(service.request("context-fixture#1", guard, "begin", {}, 0, sequence.load(), encode_manifest(source.manifest)).status == 409,
                "A missing security context did not fail closed");
        }
        require(!fs::exists(root / L"failed-context"), "A failed security context created a file stage");
        std::cout << "PASS: unavailable user scope fails closed before file I/O\n";
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    require(root.parent_path() == parent, "Unsafe context fixture cleanup target");
    fs::remove_all(root);
    std::cout << "All isolated clipboard file security-context checks passed\n";
    return 0;
}
