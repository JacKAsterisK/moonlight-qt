#pragma once
#include <memory>
#include <optional>

class NvComputer;
class ClipboardCoordinator;

// Windows Explorer copy semantics. No clipboard or filesystem wait runs on the
// streaming thread; received files appear only after every hash is verified.
class ClipboardFileSync
{
public:
    enum class Status { Preparing, Transferring, Verifying, Ready, Failed, Unavailable };
    explicit ClipboardFileSync(const NvComputer* computer, int wakeEventCode, std::shared_ptr<ClipboardCoordinator> coordinator = {});
    ~ClipboardFileSync();
    void pump();
    void notifyLocalClipboardChange();
    std::optional<Status> takeStatus();
private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
