#pragma once

#include <memory>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QSslConfiguration>
#include <QUrl>

class NvComputer;
class ClipboardCoordinator;

// The SDL thread owns the clipboard. Networking runs on a private Qt thread
// because the main Qt event loop is suspended while a stream is running.
class ClipboardSync
{
public:
    struct Connection {
        QUrl origin;
        QSslCertificate pinnedCertificate;
        QSslConfiguration clientIdentity;
        QString uniqueId;
    };
    using ReadClipboard = std::function<std::optional<QByteArray>()>;
    // Compare expected native generation while holding the clipboard, and
    // return the new generation captured before releasing it. nullopt means
    // busy or changed; the caller must sample again before retrying.
    using WriteClipboard = std::function<std::optional<quint64>(const QByteArray&, quint64)>;
    using ReadSequence = std::function<quint64()>;

    explicit ClipboardSync(const NvComputer* computer, int wakeEventCode,
                           std::shared_ptr<ClipboardCoordinator> coordinator = {});
    ClipboardSync(Connection connection, int wakeEventCode, ReadClipboard read, WriteClipboard write,
                  ReadSequence sequence = {}, std::shared_ptr<ClipboardCoordinator> coordinator = {});
    ~ClipboardSync();

    // Call on the SDL main thread. This never waits for network I/O.
    void pump();
    void notifyLocalClipboardChange();

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
