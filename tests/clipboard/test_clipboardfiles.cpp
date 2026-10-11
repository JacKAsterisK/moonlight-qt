#include "../../app/backend/clipboardfilesengine.h"
#ifdef CLIPBOARD_FILES_SERVICE_TEST
#include "clipboard_files.h"
#endif
#include <future>
#include <iostream>
#include <thread>

using namespace clipboard_files;
namespace fs = std::filesystem;
static unsigned checks = 0;
static void check(bool value, const char* message) { require(value, message); checks++; }
template<class Action> static void rejects(Action action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}
static void write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size()); require(bool(file), "Fixture write failed");
}
static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}};
}
#ifdef CLIPBOARD_FILES_SERVICE_TEST
static response_t poll(service_t& service, const std::string& owner, const guard_t& guard, const std::string& id) {
    for (unsigned i = 0; i < 1000; i++) {
        auto result = service.request(owner, guard, "status", id, 0, UINT32_MAX, {});
        if (result.status != 202) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Async service test timeout");
}
#endif

int main() {
    std::cout << std::unitbuf;
    auto fixtureParent = fs::temp_directory_path();
    if (fixtureParent.filename().empty()) fixtureParent = fixtureParent.parent_path();
    fs::path root = fixtureParent / fs::path(wide("clipboard-files-test-" + random_id()));
    fs::create_directories(root);
    try {
        manifest_t minimal {{entry_t {"ok.txt", false, 0, {}}}, 8};
        auto wire = encode_manifest(minimal);
        std::cout << "Validating bounded manifests and paths...\n";
        check(decode_manifest(wire).entries[0].path == "ok.txt", "Manifest roundtrip");
        for (auto unsafe : {"../escape", "/root", "a\\b", "a:b", "a//b", "a/./b", "CON", "aux.txt", "COM1.ext", "LPT9", "trailing.", "trailing ", "a\x01"}) {
            rejects([&] { auto invalid = minimal; invalid.entries[0].path = unsafe; encode_manifest(invalid); }, "Unsafe path accepted");
        }
        rejects([&] { auto invalid = minimal; invalid.entries[0].path = std::string("a\0b", 3); encode_manifest(invalid); }, "Embedded NUL accepted");
        rejects([&] { auto invalid = minimal; invalid.entries[0].path = std::string("\xc0\xaf", 2); encode_manifest(invalid); }, "Invalid UTF8 accepted");
        rejects([&] { auto invalid = minimal; invalid.entries.push_back(entry_t {"OK.TXT", false, 0, {}}); encode_manifest(invalid); }, "Case collision accepted");
        rejects([&] { auto invalid = minimal; invalid.entries.push_back(entry_t {"ok.txt/child", false, 0, {}}); encode_manifest(invalid); }, "File parent accepted");
        rejects([&] { auto invalid = minimal; invalid.entries[0].path = "absent/child"; encode_manifest(invalid); }, "Missing parent accepted");
        rejects([&] { decode_manifest(wire + "x"); }, "Trailing manifest accepted");
        for (size_t size = 0; size < wire.size(); size++) rejects([&] { decode_manifest(std::string_view(wire).substr(0, size)); }, "Truncated manifest accepted");
        rejects([&] { auto invalid = minimal; invalid.entries[0].size = max_total_bytes + 1; encode_manifest(invalid); }, "Oversized transfer accepted");
        rejects([&] { source_t unsafe({fs::path(L"\\\\unavailable-server\\share\\file")}, 0); }, "UNC source accepted");
        rejects([&] { source_t unsafe({fs::path(L"\\\\?\\C:\\device-path")}, 0); }, "Caller device prefix accepted");
        rejects([&] { receiver_t unsafe(minimal, fs::path(L"\\\\unavailable-server\\share\\stages")); }, "UNC stage base accepted");

        manifest_t many;
        for (unsigned i = 0; i < max_entries; i++) many.entries.push_back(entry_t {"tiny" + std::to_string(i), false, 1, {}});
        auto manyWire = encode_manifest(many);
        check(manyWire.size() > max_wire_bytes && manyWire.size() < max_manifest_bytes, "100k manifest bound");
        check(decode_manifest(manyWire).entries.size() == max_entries, "100k manifest decode");
        check(make_plan(many).size() == 1, "Tiny files were not batched");
        const auto projectedPrefix = fs::path(L"C:\\stage") / fs::path(std::wstring(75, L'x')) / fs::path(std::wstring(32, L'a'));
        const auto manyRoots = project_drop_roots(many, projectedPrefix);
        check(manyRoots.size() == max_entries && drop_list_bytes(manyRoots) > max_wire_bytes && drop_list_bytes(manyRoots) <= max_drop_bytes,
              "100k individually selected roots still use the raw batch bound");
        const auto oversizedStorage = root / fs::path(std::wstring(190, L'x'));
        bool earlyBudgetRejected = false;
        try { receiver_t tooManyRoots(many, oversizedStorage); }
        catch (const std::exception& error) { earlyBudgetRejected = std::string(error.what()) == "Clipboard root list too large"; }
        check(earlyBudgetRejected && !fs::exists(oversizedStorage), "Receiver root projection did not reject before creating a stage");
        std::vector<fs::path> oversizedSourceRoots(max_entries, oversizedStorage / L"missing.txt");
        bool sourceBudgetRejected = false;
        try { source_t tooManyRoots(oversizedSourceRoots, 0); }
        catch (const std::exception& error) { sourceBudgetRejected = std::string(error.what()) == "Clipboard root list too large"; }
        check(sourceBudgetRejected, "Source selection did not reject its native root budget before scanning");
        check(add_drop_path_bytes(max_drop_bytes - 8, 3) == max_drop_bytes, "Exact native root budget rejected");
        rejects([&] { add_drop_path_bytes(max_drop_bytes - 8, 4); }, "Native root budget overflow accepted");
        many.entries.push_back(entry_t {"excess", false, 0, {}});
        rejects([&] { encode_manifest(many); }, "Entry count cap missing");
        many = {};

        fs::create_directories(root / L"source" / L"folder" / L"nested" / L"empty");
        std::cout << "Copying staged file batches and testing retention...\n";
        auto sourceRoot = root / L"source" / L"folder";
        write(sourceRoot / L"zero.bin", {});
        for (unsigned i = 0; i < 1200; i++) write(sourceRoot / fs::path(wide("tiny" + std::to_string(i))), "tiny-content-" + std::to_string(i));
        std::string large(10 * 1024 * 1024 + 17, 0);
        for (size_t i = 0; i < large.size(); i++) large[i] = static_cast<char>(i * 31);
        write(sourceRoot / L"nested" / L"large.bin", large);
        source_t source({sourceRoot}, 42);
        check(source.plan.size() >= 3, "Large file did not split");
        receiver_t receiver(decode_manifest(encode_manifest(source.manifest)), root / L"stages");
        rejects([&] { receiver.verify(); }, "Incomplete stage verified");
        std::vector<std::future<void>> lanes;
        for (uint32_t batch = 0; batch < source.plan.size(); batch++) lanes.push_back(std::async(std::launch::async, [&, batch] { receiver.accept_batch(batch, source.read_batch(batch)); }));
        for (auto& lane : lanes) lane.get();
        receiver.verify();
        check(read(receiver.directory / L"folder" / L"nested" / L"large.bin") == large, "Large file bytes changed");
        check(fs::is_directory(receiver.directory / L"folder" / L"nested" / L"empty"), "Empty directory lost");
        check(fs::file_size(receiver.directory / L"folder" / L"zero.bin") == 0, "Zero-byte file lost");
        check(read(receiver.directory / L"folder" / L"tiny1199") == "tiny-content-1199", "Tiny file bytes changed");
        {
            auto deepRoot = root / L"long-source";
            auto deep = deepRoot;
            for (unsigned i = 0; i < 12; i++) deep /= L"long-component-name-12345";
            fs::create_directories(native_path(deep));
            auto deepFile = deep / L"payload.txt";
            write(native_path(deepFile), "long local file");
            check(deepFile.native().size() > MAX_PATH, "Long path fixture too short");
            source_t longSource({deepRoot}, 90);
            receiver_t longReceiver(longSource.manifest, root / L"long-stages");
            for (uint32_t i = 0; i < longSource.plan.size(); i++) longReceiver.accept_batch(i, longSource.read_batch(i));
            longReceiver.verify();
            check(read(native_path(longReceiver.directory / L"long-source" / deepFile.lexically_relative(deepRoot))) == "long local file", "Long file path bytes changed");
        }
        receiver.retain();
        auto retainedDirectory = receiver.directory;
        auto retainedMarker = retainedDirectory; retainedMarker += L".received";
        fs::last_write_time(retainedMarker, fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));
        auto protectedRoot = retainedDirectory / L"folder";
        auto caseVariant = protectedRoot.native();
        std::transform(caseVariant.begin(), caseVariant.end(), caseVariant.begin(), towupper);
        prune_storage(root / L"stages", {fs::path(caseVariant)});
        check(fs::exists(retainedDirectory), "Current clipboard stage cleanup ignored path case");
        prune_storage(root / L"stages", {}, [] { return true; });
        check(fs::exists(retainedDirectory), "Cleanup ignored cancellation");
        prune_storage(root / L"stages", {});
        std::cout << "Retention cleanup completed.\n";
        check(!fs::exists(retainedDirectory) && !fs::exists(retainedMarker), "Expired owned stage was not cleaned");
        rejects([&] { receiver.accept_batch(0, source.read_batch(0)); }, "Duplicate batch accepted");

        { receiver_t bad(source.manifest, root / L"stages"); auto data = source.read_batch(0); data[0] ^= 1; rejects([&] { bad.accept_batch(0, data); }, "Bad file ID accepted"); }
        { receiver_t bad(source.manifest, root / L"stages"); for (uint32_t i = 0; i < source.plan.size(); i++) { auto data = source.read_batch(i); if (!i) data[16] ^= 1; bad.accept_batch(i, data); } rejects([&] { bad.verify(); }, "Corrupt bytes verified"); }
        rejects([&] { source_t cancelled({sourceRoot}, 0, [] { return true; }); }, "Cancelled snapshot completed");
        rejects([&] { receiver_t cancelled(source.manifest, root / L"stages", [] { return true; }); }, "Cancelled stage created");
        write(sourceRoot / L"nested" / L"large.bin", "changed source size");
        bool changedRejected = false;
        for (uint32_t batch = 0; batch < source.plan.size(); batch++) { try { source.read_batch(batch); } catch (...) { changedRejected = true; } }
        check(changedRejected, "Changed source accepted");

        auto benchmarkStart = std::chrono::steady_clock::now();
        std::vector<fs::path> tinyRoots;
        for (unsigned i = 0; i < 1200; i++) tinyRoots.push_back(sourceRoot / fs::path(wide("tiny" + std::to_string(i))));
        source_t tinySource(tinyRoots, 0);
        auto scanned = std::chrono::steady_clock::now();
        receiver_t tinyReceiver(tinySource.manifest, root / L"benchmark-stage");
        auto staged = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < tinySource.plan.size(); i++) tinyReceiver.accept_batch(i, tinySource.read_batch(i));
        tinyReceiver.verify();
        auto verified = std::chrono::steady_clock::now();
        std::cout << "Local1200tinyfiles: scan/hash=" << std::chrono::duration_cast<std::chrono::milliseconds>(scanned - benchmarkStart).count()
                  << "ms, stage=" << std::chrono::duration_cast<std::chrono::milliseconds>(staged - scanned).count()
                  << "ms, batchedcopy/verify=" << std::chrono::duration_cast<std::chrono::milliseconds>(verified - staged).count()
                  << "ms, batches=" << tinySource.plan.size() << " (noWAN/TLS benchmark).\n";

        auto link = root / L"junction";
        if (CreateSymbolicLinkW(link.c_str(), sourceRoot.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 2)) {
            rejects([&] { source_t unsafe({link}, 0); }, "Source reparse accepted");
            rejects([&] { receiver_t unsafe(source.manifest, link / L"new-stage-parent"); }, "Stage reparse accepted");
            check(!fs::exists(sourceRoot / L"new-stage-parent"), "Stage wrote through reparse before rejecting");
            fs::remove(link);
        }
        else std::cout << "Symlink fixture skipped: Windows denied unprivileged symlink creation.\n";

#ifdef CLIPBOARD_FILES_SERVICE_TEST
        std::cout << "Testing authenticated async host lifecycle...\n";
        // The actual Sunshine service uses injected desktop callbacks, so no
        // test opens or modifies the user's real Windows clipboard.
        std::atomic<uint32_t> hostSequence {100};
        std::atomic<bool> authorized {true};
        std::atomic<unsigned> publications {0}, reads {0};
        std::vector<fs::path> published;
        desktop_t desktop;
        desktop.sequence = [&] { return hostSequence.load(); };
        desktop.read = [&] { reads++; return clipboard_snapshot_t {hostSequence.load(), {sourceRoot / L"zero.bin"}}; };
        desktop.publish = [&](const std::vector<fs::path>& paths, uint32_t expected) { require(hostSequence == expected, "Test stale clipboard"); published = paths; publications++; return ++hostSequence; };
        service_t service(root / L"host-stage", desktop);
        guard_t guard = [&](const std::function<void()>& action) { if (!authorized) return false; action(); return true; };
        authorized = false;
        check(service.request("owner#1", guard, "offer", {}, 0, UINT32_MAX, {}).status == 403 && reads == 0, "Unauthorized offer inspected clipboard");
        authorized = true;
        auto destination = service.request("owner#1", guard, "sequence", {}, 0, UINT32_MAX, {});
        check(destination.status == 204 && destination.sequence == hostSequence && reads == 0, "Destination baseline exported host clipboard files");
        check(service.request("owner#1", guard, "begin", {}, 0, 0, "invalid").status == 409, "Invalid upload manifest accepted");
        source_t uploadSource({sourceRoot / L"tiny0"}, 12);
        const auto uploadManifest = encode_manifest(uploadSource.manifest);
        unsigned scanChecks = 0;
        source_t racingSource({sourceRoot}, 99, [&] {
            if (++scanChecks == 10) hostSequence++; // A newer host copy occurs during real file enumeration/hashing.
            return false;
        });
        check(scanChecks > 10, "Source scan race fixture did not run");
        auto rejectedBegin = service.request("owner#1", guard, "begin", {}, 0, destination.sequence, uploadManifest);
        check(rejectedBegin.status == 409 && rejectedBegin.id.empty() && publications == 0 && !fs::exists(root / L"host-stage"), "Pre-scan host copy was overwritten or allocated a stage");
        hostSequence--;
        auto begin = service.request("owner#1", guard, "begin", {}, 0, destination.sequence, uploadManifest);
        check(begin.status == 202 && valid_id(begin.id), "Async begin failed");
        check(poll(service, "owner#1", guard, begin.id).status == 200, "Stage prepare failed");
        check(service.request("owner#2", guard, "status", begin.id, 0, 0, {}).status == 409, "Cross-owner transfer ID leaked");
        check(service.request("owner#1", guard, "batch", begin.id, 0, 0, uploadSource.read_batch(0)).status == 200, "Host batch failed");
        check(service.request("owner#1", guard, "commit", begin.id, 0, begin.sequence + 1, {}).status == 409, "Wrong sequence commit accepted");
        check(service.request("owner#1", guard, "commit", begin.id, 0, begin.sequence, {}).status == 202, "Host commit failed");
        auto complete = poll(service, "owner#1", guard, begin.id);
        check(complete.status == 200 && publications == 1 && complete.sequence == 101 && read(published[0]) == "tiny-content-0", "Verified host publish failed");

        auto stale = service.request("owner#1", guard, "begin", {}, 0, hostSequence, encode_manifest(uploadSource.manifest));
        check(poll(service, "owner#1", guard, stale.id).status == 200, "Stale fixture stage failed");
        check(service.request("owner#1", guard, "batch", stale.id, 0, 0, uploadSource.read_batch(0)).status == 200, "Stale fixture batch failed");
        hostSequence++;
        check(service.request("owner#1", guard, "commit", stale.id, 0, stale.sequence, {}).status == 202, "Stale async check not scheduled");
        check(poll(service, "owner#1", guard, stale.id).status == 409 && publications == 1, "Stale clipboard overwritten");
        authorized = false;
        check(service.request("owner#1", guard, "status", stale.id, 0, 0, {}).status == 403, "Revoked session remained authorized");
        authorized = true;

        response_t offer;
        for (unsigned i = 0; i < 1000; i++) {
            offer = service.request("owner#1", guard, "offer", {}, 0, UINT32_MAX, {});
            if (offer.status != 202) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(offer.status == 200 && valid_id(offer.id) && decode_manifest(offer.body).entries[0].size == 0, "Host download offer failed");
        check(service.request("owner#1", guard, "offer", {}, 0, hostSequence, {}).status == 204, "Unchanged clipboard offer repeated");
        check(service.request("owner#1", guard, "cancel", offer.id, 0, 0, {}).status == 200, "Host cancel failed");
        check(service.request("owner#1", guard, "batch", offer.id, 0, 0, {}).status == 409, "Cancelled transfer continued");
#endif
        std::cout << checks << " clipboard file security/state/roundtrip checks passed; fixture=1200 tiny files +10MiB, 100k manifest.\n";
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << "\n"; return 1; }
    // Only delete this freshly generated fixture directory under Windows Temp.
    try {
        require(root.parent_path() == fixtureParent, "Unsafe test cleanup target");
        fs::remove_all(native_path(root));
    }
    catch (const std::exception& error) { std::cerr << "Fixture cleanup failed: " << error.what() << '\n'; return 1; }
    return 0;
}
