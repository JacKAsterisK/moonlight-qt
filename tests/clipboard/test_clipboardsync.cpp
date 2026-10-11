#define SDL_MAIN_HANDLED
#include "backend/clipboardsync.h"
#include "backend/clipboardcoordinator.h"
#include "backend/identitymanager.h"
#include "SDL_compat.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QPointer>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QThread>
#include <QUrlQuery>

#include <functional>
#include <future>
#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

// Linking the production default constructor must not allow the fixture to read
// a saved Moonlight identity. Only the injected constructor is used below.
IdentityManager* IdentityManager::get()
{
    throw std::runtime_error("The fixture attempted to access a saved identity");
}
QSslConfiguration IdentityManager::getSslConfig()
{
    throw std::runtime_error("The fixture attempted to access a saved SSL identity");
}
QString IdentityManager::getUniqueId()
{
    throw std::runtime_error("The fixture attempted to access a saved unique ID");
}

namespace {
constexpr int MaximumTextBytes = 1024 * 1024;

void require(bool result, const char* description)
{
    if (!result) {
        throw std::runtime_error(description);
    }
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read ephemeral TLS fixture material");
    return file.readAll();
}

struct Certificates {
    QSslCertificate authority, server, selfSignedServer, wrongServer, client;
    QSslKey serverKey, clientKey;

    explicit Certificates(const QString& directory) :
        authority(readFile(directory + "/authority.pem"), QSsl::Pem),
        server(readFile(directory + "/server.pem"), QSsl::Pem),
        selfSignedServer(readFile(directory + "/self-signed-server.pem"), QSsl::Pem),
        wrongServer(readFile(directory + "/wrong-server.pem"), QSsl::Pem),
        client(readFile(directory + "/client.pem"), QSsl::Pem),
        serverKey(readFile(directory + "/server.key"), QSsl::Rsa, QSsl::Pem),
        clientKey(readFile(directory + "/client.key"), QSsl::Rsa, QSsl::Pem)
    {
        require(!server.isNull() && !selfSignedServer.isNull() && !wrongServer.isNull() && !client.isNull(), "Invalid ephemeral certificates");
        require(!serverKey.isNull() && !clientKey.isNull(), "Invalid ephemeral keys");
    }
};

// This listener implements the public HTTPS clipboard protocol, not worker
// internals. Every accepted request must present the ephemeral client identity.
class MockHost : public QTcpServer {
public:
    explicit MockHost(const Certificates& certificates, bool selfSigned = false) :
        m_Certificates(certificates), m_ServerCertificate(selfSigned ? certificates.selfSignedServer : certificates.server)
    {
        require(listen(QHostAddress::LocalHost, 0), "Cannot listen on loopback for HTTPS tests");
    }

    enum class ReplyMode { Normal, Redirect, Oversize, InvalidUtf8, EmbeddedNull };
    ReplyMode mode = ReplyMode::Normal;
    bool supported = true;
    bool holdNextGet = false;
    bool holdNextPost = false;
    int failNextPosts = 0;
    int serverInfoGets = 0, clipboardGets = 0, clipboardPosts = 0, redirectHits = 0;
    int authenticatedConnections = 0;
    bool invalidRequest = false;
    bool sendSequence = true;
    int conflictingPosts = 0;
    QString lastPostSince;
    QString heldPostSince;
    QByteArray heldPostBody;
    std::vector<QString> getCursors;
    QString tlsDiagnostic;
    QByteArray text = "host-initial";
    QByteArray lastPost;
    quint64 sequence = 1;
    QPointer<QSslSocket> held;
    QPointer<QSslSocket> heldPost;

    QUrl origin() const
    {
        QUrl url;
        url.setScheme("https");
        url.setHost("127.0.0.1");
        url.setPort(serverPort());
        return url;
    }

    QSslCertificate certificate() const { return m_ServerCertificate; }

    void changeText(QByteArray replacement)
    {
        text = std::move(replacement);
        sequence++;
    }

    void releaseHeld(QByteArray snapshot)
    {
        require(!held.isNull(), "No held clipboard GET to release");
        auto socket = held;
        held.clear();
        if (socket->state() != QAbstractSocket::UnconnectedState && socket->isEncrypted()) {
            respond(socket, 200, snapshot, sequenceHeader());
        }
    }

    void failHeldPost()
    {
        if (!heldPost.isNull() && heldPost->state() != QAbstractSocket::UnconnectedState) {
            auto socket = heldPost;
            heldPost.clear();
            respond(socket, 503, {});
        }
    }

    void conflictHeldPost()
    {
        require(!heldPost.isNull() && heldPost->state() != QAbstractSocket::UnconnectedState,
            "No live held POST to reject after a new host copy");
        require(heldPostSince != QString::number(sequence), "Held POST did not use an older host cursor");
        auto socket = heldPost;
        heldPost.clear();
        conflictingPosts++;
        respond(socket, 409, {}, sequenceHeader());
    }

    void commitHeldPost(bool loseResponse)
    {
        require(!heldPost.isNull() && heldPost->state() != QAbstractSocket::UnconnectedState,
            "A newer text copy aborted the older POST before its completion cursor");
        require(heldPostSince == QString::number(sequence), "Held POST cannot commit against a stale host cursor");
        auto socket = heldPost;
        heldPost.clear();
        changeText(heldPostBody);
        if (loseResponse) socket->abort();
        else respond(socket, 200, {}, sequenceHeader());
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        require(socket->setSocketDescriptor(descriptor), "Cannot attach the TLS test socket");
        socket->setLocalCertificate(m_ServerCertificate);
        socket->setPrivateKey(m_Certificates.serverKey);
        auto configuration = socket->sslConfiguration();
        configuration.setCaCertificates({m_Certificates.authority});
        socket->setSslConfiguration(configuration);
        socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
        connect(socket, &QSslSocket::sslErrors, socket, [this, socket](const QList<QSslError>& errors) {
            for (const auto& error : errors) {
                tlsDiagnostic += error.errorString() + "; ";
                if (error.certificate() != m_Certificates.client) {
                    return;
                }
            }
            socket->ignoreSslErrors(errors);
        });
        connect(socket, &QSslSocket::errorOccurred, socket, [this, socket](QAbstractSocket::SocketError) {
            tlsDiagnostic += socket->errorString() + "; ";
        });
        connect(socket, &QSslSocket::encrypted, socket, [this, socket] {
            if (socket->peerCertificate() != m_Certificates.client) {
                invalidRequest = true;
                socket->abort();
            }
            else {
                authenticatedConnections++;
            }
        });
        auto bytes = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, socket, [this, socket, bytes] {
            *bytes += socket->readAll();
            const qsizetype boundary = bytes->indexOf("\r\n\r\n");
            if (boundary < 0) {
                return;
            }
            const auto lines = bytes->left(boundary).split('\n');
            const auto request = lines.first().trimmed().split(' ');
            require(request.size() == 3, "Malformed request from the clipboard worker");
            qsizetype contentLength = 0;
            for (const auto& line : lines) {
                if (line.toLower().startsWith("content-length:")) {
                    contentLength = line.mid(15).trimmed().toLongLong();
                }
            }
            if (bytes->size() < boundary + 4 + contentLength) {
                return;
            }
            const QByteArray body = bytes->mid(boundary + 4, contentLength);
            bytes->clear();
            dispatch(socket, request[0], QUrl(QString::fromLatin1(request[1])), body);
        });
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        socket->startServerEncryption();
    }

private:
    QByteArray sequenceHeader() const
    {
        return sendSequence ? "X-Sunshine-Clipboard-Sequence: " + QByteArray::number(sequence) + "\r\n" : QByteArray();
    }

    void respond(QSslSocket* socket, int status, const QByteArray& body, QByteArray headers = {})
    {
        require(socket && socket->isEncrypted(), "Attempted an unencrypted fixture response");
        const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n"
            "Connection: close\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n" + headers + "\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    void dispatch(QSslSocket* socket, const QByteArray& method, const QUrl& url, const QByteArray& body)
    {
        require(socket->peerCertificate() == m_Certificates.client, "Request was not authenticated with the injected client certificate");
        const QUrlQuery query(url);
        if (url.path() == "/serverinfo" && method == "GET") {
            serverInfoGets++;
            invalidRequest |= query.queryItemValue("uniqueid") != "integration-only";
            const QByteArray capability = supported
                ? "<root><clipboardVersion>1</clipboardVersion><clipboardMaxBytes>1048576</clipboardMaxBytes></root>"
                : "<root><clipboardVersion>0</clipboardVersion><clipboardMaxBytes>0</clipboardMaxBytes></root>";
            respond(socket, 200, capability);
            return;
        }
        if (url.path() == "/redirected") {
            redirectHits++;
            respond(socket, 200, "must-not-be-followed");
            return;
        }
        invalidRequest |= url.path() != "/actions/clipboard" || query.queryItemValue("type") != "text";
        if (method == "POST") {
            clipboardPosts++;
            lastPost = body;
            lastPostSince = query.queryItemValue("since");
            if (holdNextPost) {
                holdNextPost = false;
                heldPost = socket;
                heldPostSince = lastPostSince;
                heldPostBody = body;
                return;
            }
            if (sendSequence && lastPostSince != QString::number(sequence)) {
                conflictingPosts++;
                respond(socket, 409, {}, sequenceHeader());
                return;
            }
            if (failNextPosts > 0) {
                failNextPosts--;
                respond(socket, 503, {});
            }
            else {
                changeText(body);
                respond(socket, 200, {}, sequenceHeader());
            }
            return;
        }
        clipboardGets++;
        getCursors.push_back(query.queryItemValue("since"));
        if (holdNextGet) {
            holdNextGet = false;
            held = socket;
            return;
        }
        switch (mode) {
        case ReplyMode::Redirect:
            respond(socket, 302, {}, "Location: " + origin().toEncoded() + "/redirected\r\n");
            return;
        case ReplyMode::Oversize:
            respond(socket, 200, QByteArray(MaximumTextBytes + 1, 'x'), sequenceHeader());
            return;
        case ReplyMode::InvalidUtf8:
            respond(socket, 200, QByteArray("\xc3\x28", 2), sequenceHeader());
            return;
        case ReplyMode::EmbeddedNull:
            respond(socket, 200, QByteArray("a\0b", 3), sequenceHeader());
            return;
        case ReplyMode::Normal:
            if (query.queryItemValue("since") == QString::number(sequence)) {
                respond(socket, 204, {}, sequenceHeader());
            }
            else {
                respond(socket, 200, text, sequenceHeader());
            }
        }
    }

    const Certificates& m_Certificates;
    QSslCertificate m_ServerCertificate;
};

struct MemoryClipboard {
    std::optional<QByteArray> text = QByteArray("local-initial");
    int writes = 0, writeAttempts = 0, failedWritesRemaining = 0;
    quint64 sequence = 1;
    QThread* owner = QThread::currentThread();
    std::function<void()> duringRead, beforeWrite, afterWrite;
    std::function<void()> afterSequenceRead;
    unsigned sequenceReadsUntilHook = 0;

    std::optional<QByteArray> read()
    {
        require(QThread::currentThread() == owner, "Clipboard read callback ran off the owning thread");
        auto snapshot = text;
        if (duringRead) { auto hook = std::move(duringRead); duringRead = {}; hook(); }
        return snapshot;
    }

    std::optional<quint64> write(const QByteArray& replacement, quint64 expectedSequence)
    {
        require(QThread::currentThread() == owner, "Clipboard write callback ran off the owning thread");
        writeAttempts++;
        if (beforeWrite) { auto hook = std::move(beforeWrite); beforeWrite = {}; hook(); }
        if (sequence != expectedSequence) return std::nullopt;
        if (failedWritesRemaining > 0) {
            failedWritesRemaining--;
            return std::nullopt;
        }
        text = replacement;
        sequence++;
        writes++;
        const auto publishedSequence = sequence;
        if (afterWrite) { auto hook = std::move(afterWrite); afterWrite = {}; hook(); }
        return publishedSequence;
    }

    quint64 readSequence()
    {
        const auto snapshot = sequence;
        if (afterSequenceRead && sequenceReadsUntilHook && --sequenceReadsUntilHook == 0) {
            auto hook = std::move(afterSequenceRead);
            afterSequenceRead = {};
            hook();
        }
        return snapshot;
    }
};

ClipboardSync::Connection connection(const MockHost& host, const Certificates& certificates, bool wrongPin = false)
{
    QSslConfiguration identity = QSslConfiguration::defaultConfiguration();
    identity.setLocalCertificate(certificates.client);
    identity.setPrivateKey(certificates.clientKey);
    identity.setCaCertificates({certificates.authority});
    return {host.origin(), wrongPin ? certificates.wrongServer : host.certificate(), identity, "integration-only"};
}

std::unique_ptr<ClipboardSync> createSync(MockHost& host, const Certificates& certificates, MemoryClipboard& clipboard, bool wrongPin = false,
    std::shared_ptr<ClipboardCoordinator> coordinator = {})
{
    return std::make_unique<ClipboardSync>(connection(host, certificates, wrongPin), 319,
        [&clipboard] { return clipboard.read(); },
        [&clipboard](const QByteArray& replacement, quint64 expected) { return clipboard.write(replacement, expected); },
        [&clipboard] { return clipboard.readSequence(); }, std::move(coordinator));
}

void step(ClipboardSync* sync)
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    if (sync) {
        sync->pump();
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {}
    QThread::msleep(2);
}

void spinFor(int milliseconds, ClipboardSync* sync)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        step(sync);
    }
}

void await(const std::function<bool()>& predicate, ClipboardSync* sync, const char* description)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 6000) {
        step(sync);
    }
    require(predicate(), description);
}

void testBidirectionalTextAndNoEcho(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    try {
        await([&] { return clipboard.text == host.text; }, sync.get(), "Host text did not reach the local callback");
    }
    catch (...) {
        std::cerr << "Fixture metadata: auth=" << host.authenticatedConnections << " serverinfo=" << host.serverInfoGets
                  << " get=" << host.clipboardGets << " post=" << host.clipboardPosts
                  << " TLS=" << host.tlsDiagnostic.toStdString() << '\n';
        throw;
    }
    require(clipboard.writes == 1 && host.clipboardPosts == 0, "Initial remote text echoed back as a POST");
    sync->notifyLocalClipboardChange(); // Simulate the native notification for our own successful write.
    spinFor(1100, sync.get());
    require(clipboard.writes == 1 && host.clipboardPosts == 0, "Unchanged remote text was re-written or echoed");
    clipboard.text = QByteArray("local-copied-text");
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Local copy was not sent via HTTPS POST");
    require(host.lastPost == clipboard.text && host.text == clipboard.text, "POST body did not match the local snapshot");
    spinFor(700, sync.get());
    require(host.clipboardPosts == 1 && clipboard.writes == 1, "A local copy generated a feedback loop");
    host.changeText("host-next-text");
    await([&] { return clipboard.text == host.text; }, sync.get(), "Changed host text did not reach the local callback");
    require(clipboard.writes == 2 && !host.invalidRequest && host.authenticatedConnections > 0, "Protocol or client certificate authentication failed");
}

void testRepeatedHostCopyAfterNonText(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    clipboard.text = std::nullopt;
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    spinFor(700, sync.get());
    require(!clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 0,
        "An unchanged host clipboard replaced a newer local file copy");
    host.changeText(host.text); // A deliberate new copy with the same content.
    await([&] { return clipboard.writes == 2; }, sync.get(), "A new host sequence with identical text was ignored");
    require(clipboard.text == host.text && host.clipboardPosts == 0,
        "Repeated host text did not replace non-text once without echo");
}

void testPostedSequenceSuppressesEchoAfterNonText(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    clipboard.text = QByteArray("locally-posted-text");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Local text was not posted");
    spinFor(150, sync.get());
    clipboard.text = std::nullopt;
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    spinFor(1100, sync.get());
    require(!clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 1,
        "The POST result echoed back over a newer local file copy");
    host.changeText(host.text);
    await([&] { return clipboard.writes == 2; }, sync.get(), "An explicit re-copy of the posted text was ignored");
    require(clipboard.text == host.text && host.clipboardPosts == 1,
        "Re-copy of posted text created a feedback loop");
}

void testMissingHostSequenceUsesContentFallback(const Certificates& certificates)
{
    MockHost host(certificates);
    host.sendSequence = false;
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Host without sequence headers did not deliver text");
    clipboard.text = QByteArray("copy-without-host-sequence");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Host without sequence headers did not receive text");
    spinFor(750, sync.get());
    require(clipboard.writes == 1 && host.clipboardPosts == 1 && clipboard.text == host.text,
        "Missing sequence headers disabled the content-based echo fallback");
}

void testPendingTextObservesInitialHostCursor(const Certificates& certificates)
{
    MockHost host(certificates);
    host.holdNextGet = true;
    MemoryClipboard clipboard;
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return !host.held.isNull(); }, sync.get(), "Initial host observation did not start");
    clipboard.text = QByteArray("local-copy-before-first-observation");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    spinFor(150, sync.get());
    require(coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "The shared file worker gate did not wait for pending initial text");
    require(host.clipboardPosts == 0 && clipboard.writes == 0,
        "A pending initial local copy posted before observing the host cursor");
    const auto baseline = QString::number(host.sequence);
    host.releaseHeld(host.text);
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Pending initial copy did not post after observation");
    spinFor(650, sync.get());
    require(host.lastPostSince == baseline && host.conflictingPosts == 0 && host.text == clipboard.text
        && clipboard.writes == 0 && host.clipboardPosts == 1,
        "Initial local copy did not use the observed cursor or stale host text replaced it");
    auto settlement = coordinator->takeSettlement();
    require(settlement && settlement->localSequence == clipboard.sequence && settlement->hostSequence == host.sequence
        && !settlement->conflict && !coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "Successful actual text POST did not settle the file worker's shared cursor");
    coordinator->observeLocal(static_cast<uint32_t>(clipboard.sequence), true);
    require(!coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "A delayed same-sequence file observation re-blocked an already settled text POST");
}

void testConflictingHostCopyWins(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial host text was not delivered");
    host.holdNextPost = true;
    const auto oldCursor = QString::number(host.sequence);
    clipboard.text = QByteArray("stale-local-upload");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No held guarded POST");
    require(host.heldPostSince == oldCursor, "Text POST omitted the expected host sequence");
    host.changeText(QByteArray("newer-explicit-host-copy"));
    host.conflictHeldPost();
    await([&] { return clipboard.writes == 2; }, sync.get(), "The newer host copy was lost after a 409 conflict");
    spinFor(700, sync.get());
    require(clipboard.text == host.text && host.clipboardPosts == 1 && host.conflictingPosts == 1
        && host.getCursors.back() == QString::number(host.sequence),
        "A rejected local text was retried or the newer host copy echoed");
    bool observedOldCursor = false;
    for (size_t i = 1; i < host.getCursors.size(); i++) observedOldCursor |= host.getCursors[i] == oldCursor;
    require(observedOldCursor, "409 advanced the cursor and skipped the new host selection");
}

void testNewPendingCopySurvivesPostConflict(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial host text was not delivered");
    host.holdNextPost = true;
    clipboard.text = QByteArray("older-conflicting-local-upload");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No held guarded POST");
    host.changeText(QByteArray("host-copy-during-older-upload"));
    const auto freshCursor = QString::number(host.sequence);
    host.holdNextGet = true;
    host.conflictHeldPost();
    await([&] { return !host.held.isNull(); }, sync.get(), "409 did not re-observe the host clipboard");
    clipboard.text = QByteArray("newest-copy-during-conflict-observation");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    host.releaseHeld(host.text);
    await([&] { return host.clipboardPosts == 2; }, sync.get(), "Newer pending text was lost after the older conflict");
    spinFor(700, sync.get());
    require(host.conflictingPosts == 1 && host.clipboardPosts == 2 && host.lastPostSince == freshCursor
        && host.lastPost == clipboard.text && host.text == clipboard.text && clipboard.writes == 1,
        "Conflict recovery retried old text or stale host data replaced the newest pending copy");
    auto settlement = coordinator->takeSettlement();
    require(settlement && settlement->localSequence == clipboard.sequence && settlement->hostSequence == host.sequence
        && !settlement->conflict && !coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "The older POST conflict did not preserve and settle the newest shared text copy");
}

void testNewTextPreservesOlderCommittedPost(const Certificates& certificates, bool loseResponse)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial host text was not delivered");
    host.holdNextPost = true;
    clipboard.text = QByteArray("older-post-about-to-commit");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No held older POST");
    clipboard.text = QByteArray("newest-copy-during-older-post");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    spinFor(100, sync.get());
    require(host.clipboardPosts == 1 && coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "Newer text was not kept pending while the older POST completed");
    host.commitHeldPost(loseResponse);
    const auto committedCursor = QString::number(host.sequence);
    await([&] { return host.clipboardPosts == 2; }, sync.get(), "Newer text was lost after older POST completion");
    spinFor(700, sync.get());
    require(host.clipboardPosts == 2 && host.conflictingPosts == 0 && host.lastPostSince == committedCursor
        && host.lastPost == clipboard.text && host.text == clipboard.text && clipboard.writes == 1,
        "Our older committed POST conflicted with or echoed over the newest copy");
    if (loseResponse) {
        bool observedOlderCursor = false;
        for (size_t i = 1; i < host.getCursors.size(); i++) observedOlderCursor |= host.getCursors[i] == "1";
        require(observedOlderCursor, "A lost POST response did not re-observe its possibly committed host cursor");
    }
    auto settlement = coordinator->takeSettlement();
    require(settlement && settlement->localSequence == clipboard.sequence && settlement->hostSequence == host.sequence
        && !settlement->conflict && !coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "Older completion released or replaced the newer shared text mutation");
}

void testSharedCoordinatorOrdering()
{
    ClipboardCoordinator coordinator;
    coordinator.initialize(UINT32_MAX - 1);
    require(coordinator.observeLocal(UINT32_MAX, true) && coordinator.textPending(UINT32_MAX),
        "A local text copy did not block old incoming file offers");
    require(coordinator.observeLocal(0, true) && coordinator.textPending(0),
        "Native clipboard sequence wrap lost the newest local text copy");
    coordinator.settleText(UINT32_MAX, 44); // An older asynchronous POST completes after the new copy.
    require(coordinator.textPending(0) && !coordinator.takeSettlement(),
        "An old text completion settled the newer copy");
    require(!coordinator.observeLocal(UINT32_MAX, false) && coordinator.textPending(0),
        "An old file worker observation cancelled a newer pending text copy");
    coordinator.settleText(0, 45);
    const auto settlement = coordinator.takeSettlement();
    require(settlement && settlement->hostSequence == 45 && settlement->localSequence == 0 && !settlement->conflict,
        "The shared file cursor did not receive the newest POST sequence");
    require(!coordinator.takeSettlement(), "A text settlement was delivered twice");
    coordinator.observeLocal(0, true);
    require(!coordinator.textPending(0), "A delayed repeated observation re-blocked settled text");
    require(coordinator.observeLocal(1, true) && coordinator.textPending(1), "The next local copy was suppressed");
    coordinator.settleText(1, {}, true);
    const auto conflict = coordinator.takeSettlement();
    require(conflict && conflict->conflict && !conflict->hostSequence && !coordinator.textPending(1),
        "A conflict did not release the incoming file gate for a fresh host observation");
    coordinator.observeRemote(2);
    require(!coordinator.observeLocal(2, true) && !coordinator.textPending(2),
        "An incoming text/file publication was treated as a new local mutation");
    require(coordinator.observeLocal(3, false) && !coordinator.textPending(3),
        "A local file copy was incorrectly blocked as pending text");
    coordinator.disableText();
    coordinator.observeLocal(4, true);
    require(!coordinator.textPending(4), "Unsupported text synchronization left file offers blocked");

    ClipboardCoordinator observations;
    observations.initialize(10);
    observations.observeLocal(11, false, true); // SDL rejects oversized/unusable Unicode text.
    observations.observeLocal(11, true); // File worker only knows CF_UNICODETEXT is present.
    require(!observations.textPending(11), "A file format guess re-blocked text rejected by the SDL owner");
    observations.observeLocal(12, true); // File worker observes before the SDL owner.
    require(observations.textPending(12), "An initial file format guess failed to protect pending text");
    observations.observeLocal(12, false, true);
    require(!observations.textPending(12), "Authoritative non-text observation did not clear a format guess");
    observations.observeLocal(13, true, true);
    observations.observeLocal(13, false); // A late file worker snapshot must not clear actual text.
    require(observations.textPending(13), "A late file format guess cleared actual pending text");
}

void testSharedCoordinatorPublicationGuard()
{
    ClipboardCoordinator coordinator;
    coordinator.initialize(20);
    coordinator.observeLocal(20, true, true);
    unsigned calls = 0;
    auto publish = [&]() -> std::optional<uint32_t> { calls++; return 21; };
    require(!coordinator.publishIncoming(20, publish) && calls == 0,
        "Same-generation pending text did not block incoming file publication");
    coordinator.settleText(20, 100);
    require(!coordinator.publishIncoming(19, publish) && calls == 0,
        "A stale expected native generation reached the publication callback");
    require(!coordinator.publishIncoming(20, [&]() -> std::optional<uint32_t> { calls++; return std::nullopt; }) && calls == 1,
        "A busy native publication was recorded as a remote clipboard change");
    require(coordinator.publishIncoming(20, publish) == 21 && calls == 2
        && !coordinator.observeLocal(21, true, true) && !coordinator.textPending(21),
        "Settled text did not permit publication or its native generation echoed as a new local copy");

    ClipboardCoordinator concurrent;
    concurrent.initialize(30);
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future().share();
    std::atomic<bool> observationStarted {false};
    auto publication = std::async(std::launch::async, [&] {
        return concurrent.publishIncoming(30, [&]() -> std::optional<uint32_t> {
            entered.set_value();
            releaseFuture.wait();
            return 31;
        });
    });
    const bool reachedPublication = enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!reachedPublication) {
        release.set_value();
        publication.get();
        require(false, "Concurrent publication did not reach its callback");
    }
    auto observation = std::async(std::launch::async, [&] {
        observationStarted = true;
        return concurrent.observeLocal(30, true, true);
    });
    QElapsedTimer timer;
    timer.start();
    while (!observationStarted && timer.elapsed() < 1000) step(nullptr);
    const bool serialized = observationStarted && observation.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
    release.set_value(); // Release and join before any assertion can throw.
    const auto published = publication.get();
    const auto observed = observation.get();
    require(serialized && published == 31 && !observed && !concurrent.textPending(31),
        "A same-generation local text observation raced publication or re-blocked an older native copy");
}

void testNonTextCopyCancelsDelayedFailedPost(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.holdNextPost = true;
    clipboard.text = QByteArray("text-before-file-copy");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No delayed POST to cancel");
    clipboard.text = std::nullopt;
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    host.failHeldPost(); // The failure can race the local cancellation on the worker.
    spinFor(1400, sync.get());
    require(host.clipboardPosts == 1 && clipboard.writes == 1 && !clipboard.text,
        "A delayed failed text POST was retried after a newer non-text copy");
}

void testNewerTextWinsDuringDelayedFailedPost(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.holdNextPost = true;
    clipboard.text = QByteArray("older-text-copy");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No delayed old-text POST");
    clipboard.text = QByteArray("newest-text-copy");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    host.failHeldPost();
    await([&] { return host.clipboardPosts == 2; }, sync.get(), "Newer text was lost during a failed old POST");
    spinFor(700, sync.get());
    require(host.lastPost == clipboard.text && host.text == clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 2,
        "Old queued text replaced or echoed over the newest text");
}

void testNonTextCopyAbortsInFlightPost(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.holdNextPost = true;
    clipboard.text = QByteArray("text-before-cancel");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return !host.heldPost.isNull(); }, sync.get(), "No in-flight POST to abort");
    clipboard.text = std::nullopt;
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.heldPost.isNull() || host.heldPost->state() == QAbstractSocket::UnconnectedState; },
        sync.get(), "A non-text copy did not abort the older in-flight POST");
    spinFor(700, sync.get());
    require(host.clipboardPosts == 1 && clipboard.writes == 1 && !clipboard.text,
        "An aborted old POST was retried after a file copy");
}

void testNewCopySupersedesQueuedTextDuringGet(const Certificates& certificates, bool nonText)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.holdNextGet = true;
    await([&] { return !host.held.isNull(); }, sync.get(), "No delayed GET to queue local text behind");
    clipboard.text = QByteArray("queued-older-copy");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    spinFor(80, sync.get()); // Allow the old text submission to reach the busy worker.
    clipboard.text = nonText ? std::nullopt : std::optional<QByteArray>(QByteArray("newest-queued-copy"));
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    host.releaseHeld(host.text);
    if (nonText) {
        spinFor(1200, sync.get());
        require(host.clipboardPosts == 0 && clipboard.writes == 1 && !clipboard.text,
            "Old queued text was posted after a newer file copy");
    }
    else {
        await([&] { return host.clipboardPosts == 1; }, sync.get(), "The newest queued text was lost behind a GET");
        spinFor(700, sync.get());
        require(host.clipboardPosts == 1 && clipboard.writes == 1 && host.lastPost == clipboard.text && host.text == clipboard.text,
            "Old queued text or a stale GET replaced the newer text");
    }
}

void testStaleGetCannotOverwriteLocalCopy(const Certificates& certificates, bool notify, bool nonText)
{
    MockHost host(certificates);
    host.holdNextGet = true;
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return !host.held.isNull(); }, sync.get(), "The fixture did not observe the held GET");
    const QByteArray snapshot = host.text;
    if (notify) {
        clipboard.text = nonText ? std::nullopt : std::optional<QByteArray>(QByteArray("new-local-copy"));
        sync->notifyLocalClipboardChange();
        host.releaseHeld(snapshot);
    }
    else {
        sync->pump();
        host.releaseHeld(snapshot);
        spinFor(80, nullptr); // Network callback queues delivery while the SDL pump is suspended.
        clipboard.text = QByteArray("new-local-copy");
        sync->pump();
    }
    spinFor(750, sync.get());
    require(clipboard.writes == 0, "A stale GET overwrote a newer local clipboard change");
    if (!nonText) {
        require(host.lastPost == clipboard.text && host.clipboardPosts == 1, "The newer local copy did not win and post once");
    }
    else {
        require(!clipboard.text && host.clipboardPosts == 0, "A non-text clipboard change was posted or replaced");
    }
}

void testUnsupportedCapability(const Certificates& certificates)
{
    MockHost host(certificates);
    host.supported = false;
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return host.serverInfoGets == 1; }, sync.get(), "Capability request was not received");
    clipboard.text = QByteArray("new-local-copy");
    sync->notifyLocalClipboardChange();
    spinFor(1200, sync.get());
    require(host.clipboardGets == 0 && host.clipboardPosts == 0 && clipboard.writes == 0, "Unsupported host exchanged clipboard data");
}

void testSameFormatNonTextCopyCancelsQueuedDelivery(const Certificates& certificates)
{
    MockHost host(certificates);
    host.holdNextGet = true;
    MemoryClipboard clipboard;
    clipboard.text = std::nullopt;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return !host.held.isNull(); }, sync.get(), "No held GET for same-format clipboard change");
    host.releaseHeld(host.text);
    spinFor(80, nullptr);
    clipboard.sequence++; // File/image changed while its text representation remains nullopt.
    sync->pump();
    spinFor(750, sync.get());
    require(clipboard.writes == 0 && !clipboard.text && host.clipboardPosts == 0,
        "A same-format non-text copy was overwritten by a queued GET");
}

void testCertificateMismatch(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard, true);
    clipboard.text = QByteArray("new-local-copy");
    sync->notifyLocalClipboardChange();
    spinFor(1400, sync.get());
    require(host.serverInfoGets == 0 && host.clipboardGets == 0 && host.clipboardPosts == 0 && clipboard.writes == 0,
        "An unpaired certificate received a request or delivered clipboard data");
}

void testPreviouslyPairedSelfSignedCertificate(const Certificates& certificates)
{
    MockHost host(certificates, true);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    try {
        await([&] { return clipboard.writes == 1; }, sync.get(), "Previously paired self-signed host was rejected");
    }
    catch (...) {
        std::cerr << "Self-signed fixture metadata: auth=" << host.authenticatedConnections << " serverinfo=" << host.serverInfoGets
                  << " get=" << host.clipboardGets << " post=" << host.clipboardPosts
                  << " TLS=" << host.tlsDiagnostic.toStdString() << '\n';
        throw;
    }
    require(clipboard.text == host.text && host.authenticatedConnections > 0 && !host.invalidRequest,
        "Pinned self-signed host did not deliver authenticated text");
}

void testRejectsUnsafeResponse(const Certificates& certificates, MockHost::ReplyMode mode)
{
    MockHost host(certificates);
    host.mode = mode;
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return host.clipboardGets > 0; }, sync.get(), "Unsafe-response test never reached clipboard GET");
    spinFor(350, sync.get());
    require(clipboard.writes == 0 && host.redirectHits == 0 && host.clipboardPosts == 0, "Unsafe clipboard response was applied or its redirect followed");
}

void testPostRetriesWithoutEcho(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.failNextPosts = 1;
    clipboard.text = QByteArray("retry-local-copy");
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 2; }, sync.get(), "A transient failed POST was not retried");
    spinFor(600, sync.get());
    require(host.lastPost == clipboard.text && host.text == clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 2,
        "Retry lost the local text or created an echo");
}

void testBusyLocalClipboardRetries(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    clipboard.failedWritesRemaining = 1;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "A busy local clipboard delivery was not retried");
    require(clipboard.text == host.text && host.clipboardPosts == 0, "Busy-clipboard retry corrupted text or echoed it");
}

void testNativeCopyImmediatelyBeforePublication(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    clipboard.beforeWrite = [&] { clipboard.text = std::nullopt; clipboard.sequence++; };
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writeAttempts == 1; }, sync.get(), "No text publication reached its sequence guard");
    spinFor(900, sync.get());
    require(!clipboard.text && clipboard.writes == 0 && clipboard.writeAttempts == 1 && host.clipboardPosts == 0,
        "A file copy made between sampling and publication was overwritten immediately or on retry");
    host.changeText(QByteArray("new-host-copy-after-native-file"));
    await([&] { return clipboard.writes == 1; }, sync.get(), "Guard failure prevented a later explicit host copy");
    require(clipboard.text == host.text && host.clipboardPosts == 0, "Later guarded host delivery echoed");
}

void testCopyAfterPublicationUsesCapturedSequence(const Certificates& certificates)
{
    MockHost host(certificates);
    host.holdNextPost = true;
    MemoryClipboard clipboard;
    clipboard.afterWrite = [&] { clipboard.sequence++; }; // Explicit same-text copy immediately after close.
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return !host.heldPost.isNull(); }, sync.get(),
        "An external same-text copy after publication was misidentified as our remote write");
    require(clipboard.writes == 1 && host.clipboardPosts == 1
        && coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "Publication recorded a later external copy's generation instead of its captured generation");
    host.commitHeldPost(false);
    spinFor(700, sync.get());
    require(host.clipboardPosts == 1 && clipboard.writes == 1 && host.text == clipboard.text,
        "Post-publication native copy created an echo loop");
}

void testCopyDuringReadDoesNotMixSnapshot(const Certificates& certificates, bool nonText)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial text delivery failed");
    host.holdNextGet = true;
    await([&] { return !host.held.isNull(); }, sync.get(), "No pending remote result for read-time copy");
    host.changeText(QByteArray("host-result-before-read-time-copy"));
    host.releaseHeld(host.text);
    spinFor(80, nullptr); // Queue the remote delivery without pumping its owner.
    clipboard.duringRead = [&] {
        clipboard.text = nonText ? std::nullopt : std::optional<QByteArray>(QByteArray("latest-read-time-local-copy"));
        clipboard.sequence++;
    };
    sync->pump();
    require(clipboard.writes == 1 && host.clipboardPosts == 0,
        "An unstable sample published remote text or uploaded text from another generation");
    if (nonText) {
        spinFor(900, sync.get());
        require(!clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 0,
            "A pending text delivery was retried over the new file copy after an unstable read");
    }
    else {
        await([&] { return host.clipboardPosts == 1; }, sync.get(), "The stable newest copy was not resampled and posted");
        spinFor(700, sync.get());
        require(host.lastPost == clipboard.text && host.text == clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 1,
            "Text from before a read-time copy was associated with the newer generation");
    }
}

void testStartupNonTextCopyDiscardsInitialHost(const Certificates& certificates, bool constructorTear, bool sequenceHeader)
{
    MockHost host(certificates);
    host.holdNextGet = true;
    host.sendSequence = sequenceHeader;
    MemoryClipboard clipboard;
    if (constructorTear) {
        clipboard.duringRead = [&] { clipboard.text = std::nullopt; clipboard.sequence++; };
    }
    auto sync = createSync(host, certificates, clipboard);
    if (!constructorTear) {
        clipboard.text = std::nullopt;
        clipboard.sequence++;
        sync->notifyLocalClipboardChange();
    }
    else sync->pump();
    await([&] { return !host.held.isNull(); }, sync.get(), "Initial host GET did not start after the local file copy");
    host.releaseHeld(host.text);
    spinFor(900, sync.get());
    require(!clipboard.text && clipboard.writes == 0 && host.clipboardPosts == 0,
        "Pre-existing host text overwrote a startup file copy or a torn constructor observation");
    host.changeText(QByteArray("new-host-copy-after-startup-baseline"));
    await([&] { return clipboard.writes == 1; }, sync.get(), "Startup discard prevented a later explicit host copy");
    require(clipboard.text == host.text && host.clipboardPosts == 0, "Later startup host copy echoed");
}

void testFilePublicationWinsAfterSampling(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    auto coordinator = std::make_shared<ClipboardCoordinator>();
    auto sync = createSync(host, certificates, clipboard, false, coordinator);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Initial host text was not delivered");
    host.holdNextGet = true;
    await([&] { return !host.held.isNull(); }, sync.get(), "No held host GET for publication handoff");
    clipboard.text = QByteArray("sampled-text-before-file-publication");
    clipboard.sequence++;
    // The file worker observed this generation before its text format became
    // usable; the authoritative text observation will arrive after publication.
    coordinator->observeLocal(static_cast<uint32_t>(clipboard.sequence), false);
    clipboard.sequenceReadsUntilHook = 2; // Immediately after the stable read fence.
    clipboard.afterSequenceRead = [&] {
        auto published = coordinator->publishIncoming(static_cast<uint32_t>(clipboard.sequence), [&]() -> std::optional<uint32_t> {
            clipboard.text = std::nullopt;
            return static_cast<uint32_t>(++clipboard.sequence);
        });
        require(published.has_value(), "Fixture incoming file publication did not win the lock handoff");
    };
    sync->notifyLocalClipboardChange();
    host.releaseHeld(host.text);
    spinFor(1000, sync.get());
    require(!clipboard.text && clipboard.writes == 1 && host.clipboardPosts == 0,
        "A rejected stale text observation uploaded or overwrote a newer native file publication");
    require(!coordinator->textPending(static_cast<uint32_t>(clipboard.sequence)),
        "Our own file publication became an indefinitely pending text copy");
}

void testLocalCopyCancelsBusyDeliveryRetry(const Certificates& certificates)
{
    MockHost host(certificates);
    MemoryClipboard clipboard;
    clipboard.failedWritesRemaining = 1;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writeAttempts == 1; }, sync.get(), "No busy delivery to cancel with a local copy");
    clipboard.text = QByteArray("copy-after-busy-clipboard");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Copy after busy delivery did not post");
    spinFor(600, sync.get());
    require(clipboard.writes == 0 && clipboard.text == host.lastPost && host.text == clipboard.text,
        "A queued busy-clipboard retry overwrote a newer local copy");
}

void testEmptyTextTransfersBothDirections(const Certificates& certificates)
{
    MockHost host(certificates);
    host.changeText({});
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return clipboard.writes == 1; }, sync.get(), "Empty host text was not delivered");
    require(clipboard.text && clipboard.text->isEmpty(), "Empty host text did not clear the mock text clipboard");
    clipboard.text = QByteArray("text-before-clear");
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 1; }, sync.get(), "Local text before clear was not posted");
    clipboard.text = QByteArray();
    clipboard.sequence++;
    sync->notifyLocalClipboardChange();
    await([&] { return host.clipboardPosts == 2; }, sync.get(), "Empty local text was not posted");
    require(host.lastPost.isEmpty() && host.text.isEmpty(), "Empty local text did not clear the host text");
}

void testTeardownWhileRequestPending(const Certificates& certificates)
{
    MockHost host(certificates);
    host.holdNextGet = true;
    MemoryClipboard clipboard;
    auto sync = createSync(host, certificates, clipboard);
    await([&] { return !host.held.isNull(); }, sync.get(), "No request remained pending for teardown");
    QElapsedTimer timer;
    timer.start();
    sync.reset();
    require(timer.elapsed() < 1000, "Teardown waited for the network request timeout");
    spinFor(350, nullptr);
    require(clipboard.writes == 0, "Teardown allowed a late clipboard delivery");
    require(host.held.isNull() || host.held->state() == QAbstractSocket::UnconnectedState, "Teardown left the network socket active");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: test_clipboardsync <ephemeral-certificate-directory> [test-name]\n";
        return 2;
    }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        std::cerr << "Cannot initialize isolated SDL events\n";
        return 2;
    }
    try {
        require(QSslSocket::supportsSsl(), "The Qt TLS backend is unavailable");
        const Certificates certificates(QString::fromLocal8Bit(argv[1]));
        const std::vector<std::pair<const char*, std::function<void()>>> tests {
            {"bidirectional text and no echo", [&] { testBidirectionalTextAndNoEcho(certificates); }},
            {"identical host copy after non-text", [&] { testRepeatedHostCopyAfterNonText(certificates); }},
            {"POST sequence suppresses echo after non-text", [&] { testPostedSequenceSuppressesEchoAfterNonText(certificates); }},
            {"missing host sequence uses content fallback", [&] { testMissingHostSequenceUsesContentFallback(certificates); }},
            {"pending initial text observes host cursor before POST", [&] { testPendingTextObservesInitialHostCursor(certificates); }},
            {"conflicting host copy wins without stale retry", [&] { testConflictingHostCopyWins(certificates); }},
            {"new pending text survives older POST conflict", [&] { testNewPendingCopySurvivesPostConflict(certificates); }},
            {"new text preserves older committed POST cursor", [&] { testNewTextPreservesOlderCommittedPost(certificates, false); }},
            {"new text survives older committed POST lost response", [&] { testNewTextPreservesOlderCommittedPost(certificates, true); }},
            {"shared text and file coordinator ordering", [&] { testSharedCoordinatorOrdering(); }},
            {"shared incoming publication serializes pending text", [&] { testSharedCoordinatorPublicationGuard(); }},
            {"non-text copy cancels delayed failed POST", [&] { testNonTextCopyCancelsDelayedFailedPost(certificates); }},
            {"newer text wins during delayed failed POST", [&] { testNewerTextWinsDuringDelayedFailedPost(certificates); }},
            {"non-text copy aborts in-flight POST", [&] { testNonTextCopyAbortsInFlightPost(certificates); }},
            {"non-text copy supersedes queued text during GET", [&] { testNewCopySupersedesQueuedTextDuringGet(certificates, true); }},
            {"newer text supersedes queued text during GET", [&] { testNewCopySupersedesQueuedTextDuringGet(certificates, false); }},
            {"stale GET versus notified copy", [&] { testStaleGetCannotOverwriteLocalCopy(certificates, true, false); }},
            {"fresh delivery observation versus unnotified copy", [&] { testStaleGetCannotOverwriteLocalCopy(certificates, false, false); }},
            {"non-text copy cancels stale delivery", [&] { testStaleGetCannotOverwriteLocalCopy(certificates, true, true); }},
            {"same-format non-text copy cancels queued delivery", [&] { testSameFormatNonTextCopyCancelsQueuedDelivery(certificates); }},
            {"unsupported capability", [&] { testUnsupportedCapability(certificates); }},
            {"paired certificate mismatch", [&] { testCertificateMismatch(certificates); }},
            {"previously paired self-signed certificate", [&] { testPreviouslyPairedSelfSignedCertificate(certificates); }},
            {"HTTPS redirect rejected", [&] { testRejectsUnsafeResponse(certificates, MockHost::ReplyMode::Redirect); }},
            {"oversized response rejected", [&] { testRejectsUnsafeResponse(certificates, MockHost::ReplyMode::Oversize); }},
            {"invalid UTF-8 rejected", [&] { testRejectsUnsafeResponse(certificates, MockHost::ReplyMode::InvalidUtf8); }},
            {"embedded NUL rejected", [&] { testRejectsUnsafeResponse(certificates, MockHost::ReplyMode::EmbeddedNull); }},
            {"transient POST retry without echo", [&] { testPostRetriesWithoutEcho(certificates); }},
            {"busy clipboard retries delivery", [&] { testBusyLocalClipboardRetries(certificates); }},
            {"native file copy immediately before guarded publication", [&] { testNativeCopyImmediatelyBeforePublication(certificates); }},
            {"external copy after publication preserves captured sequence", [&] { testCopyAfterPublicationUsesCapturedSequence(certificates); }},
            {"file copy during sampling defers stale delivery", [&] { testCopyDuringReadDoesNotMixSnapshot(certificates, true); }},
            {"text copy during sampling uses coherent snapshot", [&] { testCopyDuringReadDoesNotMixSnapshot(certificates, false); }},
            {"startup file copy discards pre-existing host baseline", [&] { testStartupNonTextCopyDiscardsInitialHost(certificates, false, true); }},
            {"startup file copy discards headerless host baseline", [&] { testStartupNonTextCopyDiscardsInitialHost(certificates, false, false); }},
            {"constructor torn snapshot preserves startup file copy", [&] { testStartupNonTextCopyDiscardsInitialHost(certificates, true, true); }},
            {"file publication wins after stable text sampling", [&] { testFilePublicationWinsAfterSampling(certificates); }},
            {"local copy cancels busy delivery retry", [&] { testLocalCopyCancelsBusyDeliveryRetry(certificates); }},
            {"empty text transfers both directions", [&] { testEmptyTextTransfersBothDirections(certificates); }},
            {"teardown during pending request", [&] { testTeardownWhileRequestPending(certificates); }},
        };
        size_t executed = 0;
        for (const auto& test : tests) {
            if (argc == 3 && QString::fromLocal8Bit(argv[2]) != QString::fromLatin1(test.first)) {
                continue;
            }
            test.second();
            executed++;
            std::cout << "PASS: " << test.first << std::endl;
        }
        require(executed > 0, "No integration test matched the requested name");
        std::cout << "All " << executed << " isolated clipboard HTTPS integration tests passed\n";
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_Quit();
    return 0;
}
