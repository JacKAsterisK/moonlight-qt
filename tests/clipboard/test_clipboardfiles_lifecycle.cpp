#include "clipboard_files.h"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>

using namespace clipboard_files;
namespace fs = std::filesystem;

namespace {
response_t awaitStage(service_t& service, const guard_t& guard, const std::string& id)
{
    for (unsigned attempt = 0; attempt < 1000; attempt++) {
        auto result = service.request("fixture#1", guard, "status", id, 0, UINT32_MAX, {});
        if (result.status != 202) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Service lifecycle fixture timed out");
}

bool emptyStorage(const fs::path& storage)
{
    return !fs::exists(storage) || fs::is_empty(storage);
}

void requirePrivateStage(const fs::path& storage)
{
    unsigned stages = 0, markers = 0;
    for (const auto& item : fs::directory_iterator(storage)) {
        if (item.is_directory()) stages++;
        if (item.path().extension() == L".pending") markers++;
    }
    require(stages == 1 && markers == 1, "Ready unpublished stage was not isolated and marked");
}
} // namespace

int main()
{
    const fs::path root = fs::absolute(fs::path(".tools") / fs::path(wide("clipboard-files-lifecycle-" + random_id())));
    fs::create_directories(root);
    try {
        const auto sourcePath = root / L"source.txt";
        { std::ofstream fixture(sourcePath, std::ios::binary); fixture << "isolated lifecycle fixture"; require(bool(fixture), "Cannot create lifecycle fixture"); }
        source_t source({sourcePath}, 1);
        const auto manifest = encode_manifest(source.manifest);
        std::atomic<uint32_t> sequence {10};
        std::atomic<bool> authorized {true};
        std::atomic<unsigned> publications {0};
        desktop_t desktop;
        desktop.read = [&] { return clipboard_snapshot_t {sequence.load(), {}}; };
        desktop.sequence = [&] { return sequence.load(); };
        desktop.publish = [&](const std::vector<fs::path>&, uint32_t expected) {
            require(sequence == expected && authorized, "Unauthorized or stale fixture publication");
            publications++;
            return ++sequence;
        };
        guard_t guard = [&](const std::function<void()>& action) { if (!authorized) return false; action(); return true; };

        const auto shutdownStorage = root / L"shutdown";
        {
            service_t service(shutdownStorage, desktop);
            auto begin = service.request("fixture#1", guard, "begin", {}, 0, sequence.load(), manifest);
            require(begin.status == 202 && awaitStage(service, guard, begin.id).status == 200, "Stage preparation failed");
            requirePrivateStage(shutdownStorage);
            require(publications == 0, "Incomplete files were published");
        }
        require(emptyStorage(shutdownStorage), "Service destruction retained an unpublished stage or marker");
        std::cout << "PASS: shutdown removes a ready unpublished stage\n";

        const auto cancelStorage = root / L"cancel";
        {
            service_t service(cancelStorage, desktop);
            auto begin = service.request("fixture#1", guard, "begin", {}, 0, sequence.load(), manifest);
            require(begin.status == 202 && awaitStage(service, guard, begin.id).status == 200, "Cancelled fixture stage failed");
            requirePrivateStage(cancelStorage);
            require(service.request("fixture#1", guard, "cancel", begin.id, 0, UINT32_MAX, {}).status == 200, "Transfer cancellation failed");
            for (unsigned attempt = 0; attempt < 500 && !emptyStorage(cancelStorage); attempt++) {
                service.request("fixture#1", guard, "offer", {}, 0, sequence, {});
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            require(emptyStorage(cancelStorage), "Cancelled finished transfer retained its private stage while service remained alive");
            require(publications == 0, "Cancelled files were published");
        }
        std::cout << "PASS: retiring a cancelled transfer releases its stage\n";

        const auto revokeStorage = root / L"revoke";
        {
            service_t service(revokeStorage, desktop);
            auto begin = service.request("fixture#1", guard, "begin", {}, 0, sequence.load(), manifest);
            require(begin.status == 202 && awaitStage(service, guard, begin.id).status == 200, "Revocation fixture stage failed");
            require(service.request("fixture#1", guard, "batch", begin.id, 0, UINT32_MAX, source.read_batch(0)).status == 200,
                "Revocation fixture batch failed");
            std::mutex gateMutex;
            std::condition_variable gateChanged;
            bool arrived = false, released = false, revocationObserved = false;
            const auto requester = std::this_thread::get_id();
            guard_t heldGuard = [&](const std::function<void()>& action) {
                if (std::this_thread::get_id() != requester) {
                    std::unique_lock<std::mutex> lock(gateMutex);
                    arrived = true;
                    gateChanged.notify_all();
                    gateChanged.wait(lock, [&] { return released; });
                    if (!authorized) {
                        revocationObserved = true;
                        gateChanged.notify_all();
                        return false;
                    }
                }
                if (!authorized) return false;
                action();
                return true;
            };
            require(service.request("fixture#1", heldGuard, "commit", begin.id, 0, begin.sequence, {}).status == 202,
                "Revocation fixture commit failed");
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                require(gateChanged.wait_for(lock, std::chrono::seconds(5), [&] { return arrived; }), "Verification did not reach its authorization boundary");
                authorized = false;
                released = true;
                gateChanged.notify_all();
                require(gateChanged.wait_for(lock, std::chrono::seconds(5), [&] { return revocationObserved; }),
                    "Worker did not observe the revoked authorization");
            }
            require(service.request("fixture#1", guard, "status", begin.id, 0, UINT32_MAX, {}).status == 403,
                "Revoked session still accessed the transfer");
            authorized = true; // Inspect the completed failure using the fixture's permitted identity.
            require(awaitStage(service, guard, begin.id).status == 409 && publications == 0,
                "An asynchronously revoked commit published its files");
        }
        require(emptyStorage(revokeStorage), "Revoked commit retained an unpublished stage after shutdown");
        std::cout << "PASS: revocation during verification prevents publication and cleans shutdown\n";
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    require(root.parent_path() == fs::absolute(".tools"), "Unsafe lifecycle fixture cleanup target");
    fs::remove_all(root);
    std::cout << "All isolated clipboard file service lifecycle checks passed\n";
    return 0;
}
