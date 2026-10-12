#include "clipboardfilesync.h"
#include "clipboardfilesengine.h"
#include "clipboardcoordinator.h"
#include "identitymanager.h"
#include "nvcomputer.h"
#include "SDL_compat.h"

#include <QDebug>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSslError>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <condition_variable>
#include <future>
#include <optional>

#ifdef Q_OS_WIN
namespace {
using namespace clipboard_files;

struct Connection {
    QUrl origin;
    QSslCertificate certificate;
    QSslConfiguration identity;
    QString uniqueId;
};
struct Reply {
    int status = 0;
    QByteArray body;
    std::string id;
    uint32_t sequence = UINT32_MAX;
};

// Each transfer lane owns a Qt network manager on its own thread. Requests on
// a lane reuse its HTTPS connection; four lanes amortize WAN round-trip time.
Reply request(QNetworkAccessManager& network, const Connection& connection,
              const QString& operation, const std::string& id, uint32_t batch,
              uint32_t sequence, const QByteArray& body, bool post,
              const cancel_t& cancelled)
{
    QUrl url(connection.origin);
    QUrlQuery query;
    if (operation == QStringLiteral("serverinfo")) {
        url.setPath(QStringLiteral("/serverinfo"));
        query.addQueryItem(QStringLiteral("uniqueid"), connection.uniqueId);
    }
    else {
        url.setPath(QStringLiteral("/actions/clipboard/files"));
        query.addQueryItem(QStringLiteral("op"), operation);
        if (!id.empty()) query.addQueryItem(QStringLiteral("id"), QString::fromStdString(id));
        if (operation == QStringLiteral("batch")) query.addQueryItem(QStringLiteral("batch"), QString::number(batch));
        if (sequence != UINT32_MAX) query.addQueryItem(operation == QStringLiteral("offer") ? QStringLiteral("since") : QStringLiteral("sequence"), QString::number(sequence));
    }
    url.setQuery(query);
    QNetworkRequest message(url);
    message.setSslConfiguration(connection.identity);
    message.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    message.setRawHeader("Cache-Control", "no-store");
    message.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    auto* reply = post ? network.post(message, body) : network.get(message);
    const auto limit = operation == QStringLiteral("offer") ? max_manifest_bytes : max_wire_bytes;
    reply->setReadBufferSize(limit + 1);
    Reply result;
    bool trusted = false;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [&](const QList<QSslError>& errors) {
        for (const auto& error : errors) if (error.certificate() != connection.certificate) return;
        reply->ignoreSslErrors(errors);
    });
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [&] {
        trusted = reply->sslConfiguration().peerCertificate() == connection.certificate;
        if (!trusted) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&] {
        result.body += reply->readAll();
        if (result.body.size() > limit) reply->abort();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline.start(operation == QStringLiteral("begin") || operation == QStringLiteral("offer") ? 60000 : 15000);
    QTimer cancellation;
    cancellation.setInterval(100);
    QObject::connect(&cancellation, &QTimer::timeout, reply, [&] { if (cancelled && cancelled()) reply->abort(); });
    cancellation.start();
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec();
    result.body += reply->readAll();
    // A reused TLS connection need not emit encrypted() on every request.
    trusted = trusted || reply->sslConfiguration().peerCertificate() == connection.certificate;
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (trusted && result.body.size() <= limit && (!cancelled || !cancelled()) &&
        (reply->error() == QNetworkReply::NoError || status >= 400)) {
        result.status = status;
        result.id = reply->rawHeader("X-Sunshine-Clipboard-Transfer").toStdString();
        bool ok = false;
        uint32_t value = reply->rawHeader("X-Sunshine-Clipboard-Sequence").toUInt(&ok);
        if (ok) result.sequence = value;
    }
    // All connections capture stack state; delete before leaving this request.
    delete reply;
    return result;
}

bool supportsFiles(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    QString version;
    uint32_t limit = 0;
    uint32_t manifestLimit = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QStringLiteral("clipboardFilesVersion")) version = reader.readElementText();
            else if (reader.name() == QStringLiteral("clipboardFilesMaxBatchBytes")) limit = reader.readElementText().toUInt();
            else if (reader.name() == QStringLiteral("clipboardFilesMaxManifestBytes")) manifestLimit = reader.readElementText().toUInt();
        }
    }
    return !reader.hasError() && version == QStringLiteral("1") && limit >= max_wire_bytes && manifestLimit >= max_manifest_bytes;
}

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> stopping {false};
    uint64_t generation = 0;
    clipboard_snapshot_t local;
    std::optional<ClipboardFileSync::Status> pendingStatus;
    std::optional<ClipboardFileSync::Status> lastStatus;
};

void transferBatches(const Connection& connection, const std::string& id, const plan_t& plan,
                     const source_t* source, receiver_t* receiver, const cancel_t& cancelled)
{
    std::atomic<uint32_t> next {0};
    std::atomic<bool> failed {false};
    std::vector<std::future<void>> lanes;
    std::exception_ptr failure;
    try {
      for (unsigned lane = 0; lane < std::min<size_t>(parallel_workers, plan.size()); lane++) {
        lanes.push_back(std::async(std::launch::async, [&] {
            background_t priority;
            QNetworkAccessManager network;
            network.setProxy(QNetworkProxy::NoProxy);
            auto cancel = [&] { return failed || cancelled(); };
            try {
                for (;;) {
                    require(!cancel(), "Clipboard file transfer cancelled");
                    auto index = next.fetch_add(1);
                    if (index >= plan.size()) return;
                    QByteArray body;
                    if (source) { auto wire = source->read_batch(index, cancel); body = QByteArray(wire.data(), static_cast<int>(wire.size())); }
                    auto reply = request(network, connection, QStringLiteral("batch"), id, index, UINT32_MAX, body, source != nullptr, cancel);
                    require(reply.status == 200, "Clipboard file batch failed");
                    if (receiver) receiver->accept_batch(index, std::string_view(reply.body.constData(), reply.body.size()));
                }
            }
            catch (...) { failed = true; throw; }
        }));
      }
    }
    catch (...) { failed = true; failure = std::current_exception(); }
    for (auto& lane : lanes) { try { lane.get(); } catch (...) { if (!failure) failure = std::current_exception(); } }
    if (failure) std::rethrow_exception(failure);
}
} // namespace

class ClipboardFileSync::Impl {
public:
    Impl(const NvComputer* computer, int wakeCode, std::shared_ptr<ClipboardCoordinator> sharedCoordinator) : coordinator(std::move(sharedCoordinator)), wakeCode(wakeCode) {
        connection.origin.setScheme(QStringLiteral("https"));
        connection.origin.setHost(computer->activeAddress.address());
        connection.origin.setPort(computer->activeHttpsPort);
        connection.certificate = computer->serverCert;
        connection.identity = IdentityManager::get()->getSslConfig();
        connection.uniqueId = IdentityManager::get()->getUniqueId();
        storage = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdWString()) / L"MoonlightFork" / L"Clipboard";
        observed = GetClipboardSequenceNumber();
        if (coordinator) coordinator->initialize(observed);
        state.local.sequence = observed;
        // Only a new local copy during this stream uploads files. A pre-existing
        // clipboard selection can belong to an unrelated earlier session.
        thread.reset(QThread::create([this] { run(); }));
        thread->start();
    }
    ~Impl() {
        state.stopping = true;
        state.changed.notify_all();
        thread->wait();
    }
    bool cancelled(uint64_t generation, uint32_t sequence) {
        if (state.stopping || GetClipboardSequenceNumber() != sequence) return true;
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.generation != generation;
    }
    void wake() {
        SDL_Event event {};
        event.type = SDL_USEREVENT;
        event.user.code = wakeCode;
        SDL_PushEvent(&event);
    }
    void report(ClipboardFileSync::Status status) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.lastStatus == status) return;
            state.lastStatus = status;
            state.pendingStatus = status;
        }
        wake();
    }
    void run() {
        background_t priority;
        try {
            const auto snapshot = read_clipboard_files();
            prune_storage(storage, snapshot.paths, [&] {
                return state.stopping || GetClipboardSequenceNumber() != snapshot.sequence;
            });
        } catch (...) {} // Busy clipboard or unowned stages are left intact.
        QNetworkAccessManager network;
        network.setProxy(QNetworkProxy::NoProxy);
        auto stop = [&] { return bool(state.stopping); };
        auto capability = request(network, connection, QStringLiteral("serverinfo"), {}, 0, UINT32_MAX, {}, false, stop);
        if (capability.status != 200 || !supportsFiles(capability.body)) {
            qInfo("File clipboard sync is unavailable or disabled on this host.");
            report(Status::Unavailable);
            return;
        }
        uint64_t handled = 0;
        uint32_t remoteSequence = UINT32_MAX;
        bool baselineBarrier = false;
        while (!state.stopping) {
            if (GetClipboardSequenceNumber() != observed) {
                try {
                    auto snapshot = read_clipboard_files();
                    observed = snapshot.sequence;
                    const bool textOrEmpty = snapshot.paths.empty() &&
                        (IsClipboardFormatAvailable(CF_UNICODETEXT) || CountClipboardFormats() == 0);
                    const bool localCopy = !coordinator || coordinator->observeLocal(snapshot.sequence, textOrEmpty);
                    if (localCopy) baselineBarrier = true;
                    std::lock_guard<std::mutex> lock(state.mutex);
                    state.generation++;
                    state.local = std::move(snapshot);
                }
                catch (...) { QThread::msleep(100); continue; }
            }
            clipboard_snapshot_t local;
            uint64_t generation;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                local = state.local;
                generation = state.generation;
            }
            auto cancel = [&] { return cancelled(generation, local.sequence); };
            auto cancelIncoming = [&] { return cancel() || (coordinator && coordinator->textPending(local.sequence)); };
            std::string activeId;
            try {
                if (generation != handled && !local.paths.empty()) {
                    handled = generation;
                    report(Status::Preparing);
                    auto destination = request(network, connection, QStringLiteral("sequence"), {}, 0, UINT32_MAX, {}, false, cancel);
                    require(destination.status == 204 && destination.sequence != UINT32_MAX, "Host clipboard baseline is unavailable");
                    remoteSequence = destination.sequence;
                    baselineBarrier = false;
                    source_t source(local.paths, local.sequence, cancel);
                    auto wire = encode_manifest(source.manifest);
                    auto begin = request(network, connection, QStringLiteral("begin"), {}, 0, destination.sequence, QByteArray(wire.data(), static_cast<int>(wire.size())), true, cancel);
                    require((begin.status == 200 || begin.status == 202) && valid_id(begin.id) && begin.sequence != UINT32_MAX, "Host refused clipboard file transfer");
                    activeId = begin.id;
                    while (begin.status == 202) {
                        require(!cancel(), "Clipboard file transfer cancelled");
                        auto ready = request(network, connection, QStringLiteral("status"), activeId, 0, UINT32_MAX, {}, false, cancel);
                        require(ready.status == 200 || ready.status == 202, "Host could not prepare clipboard stage");
                        begin.status = ready.status;
                        if (ready.status == 202) QThread::msleep(150);
                    }
                    report(Status::Transferring);
                    transferBatches(connection, activeId, source.plan, &source, nullptr, cancel);
                    report(Status::Verifying);
                    auto commit = request(network, connection, QStringLiteral("commit"), activeId, 0, begin.sequence, {}, true, cancel);
                    require(commit.status == 202, "Host refused clipboard file completion");
                    for (;;) {
                        require(!cancel(), "Clipboard file transfer cancelled");
                        auto status = request(network, connection, QStringLiteral("status"), activeId, 0, UINT32_MAX, {}, false, cancel);
                        if (status.status == 200) { remoteSequence = status.sequence; break; }
                        require(status.status == 202, "Host could not verify clipboard files");
                        QThread::msleep(150);
                    }
                    activeId.clear();
                    report(Status::Ready);
                }
                else if (generation != handled) handled = generation;
                if (coordinator) {
                    auto settlement = coordinator->takeSettlement();
                    if (settlement && settlement->localSequence == local.sequence) {
                        if (settlement->conflict) {
                            baselineBarrier = false;
                            remoteSequence = UINT32_MAX;
                        }
                        else if (settlement->hostSequence) {
                            baselineBarrier = false;
                            remoteSequence = *settlement->hostSequence;
                        }
                    }
                }
                const bool pendingText = coordinator && coordinator->textPending(local.sequence);
                if (!cancel() && !pendingText && baselineBarrier) {
                    // Discard the host's current selection after a new local
                    // image/text/empty copy, then receive only future changes.
                    auto baseline = request(network, connection, QStringLiteral("sequence"), {}, 0, UINT32_MAX, {}, false, cancel);
                    if (baseline.status == 204 && baseline.sequence != UINT32_MAX) {
                        remoteSequence = baseline.sequence;
                        baselineBarrier = false;
                    }
                }
                if (!cancel() && !pendingText && !baselineBarrier) {
                    auto offer = request(network, connection, QStringLiteral("offer"), {}, 0, remoteSequence, {}, false, cancelIncoming);
                    if (offer.status == 403 || offer.status == 404) {
                        qWarning("File clipboard sync was disabled by the host.");
                        report(Status::Unavailable);
                        return;
                    }
                    if (offer.status == 204) remoteSequence = offer.sequence;
                    if (offer.status == 202) report(Status::Preparing);
                    if (offer.status == 200) {
                        report(Status::Preparing);
                        require(valid_id(offer.id), "Invalid clipboard file offer identity");
                        activeId = offer.id;
                        auto manifest = decode_manifest(std::string_view(offer.body.constData(), offer.body.size()));
                        auto receiver = std::make_shared<receiver_t>(std::move(manifest), storage, cancelIncoming);
                        report(Status::Transferring);
                        transferBatches(connection, activeId, receiver->plan, nullptr, receiver.get(), cancelIncoming);
                        report(Status::Verifying);
                        receiver->verify(cancelIncoming);
                        require(!cancelIncoming(), "Clipboard changed before file delivery");
                        prepared_drop_t prepared(receiver->roots);
                        bool published = false;
                        for (unsigned attempt = 0; attempt < 20 && !cancelIncoming(); attempt++) {
                            try {
                                require(!cancelIncoming(), "Clipboard changed before file delivery");
                                auto publish = [&]() -> std::optional<uint32_t> {
                                    if (state.stopping) return std::nullopt;
                                    return prepared.publish(local.sequence);
                                };
                                auto sequence = coordinator ? coordinator->publishIncoming(local.sequence, publish) : publish();
                                require(sequence.has_value(), "Clipboard changed before file delivery");
                                receiver->retain();
                                observed = *sequence;
                                std::lock_guard<std::mutex> lock(state.mutex);
                                state.local.sequence = *sequence;
                                state.local.paths.clear();
                                handled = ++state.generation;
                                published = true;
                                break;
                            }
                            catch (...) { QThread::msleep(250); }
                        }
                        require(published, "Clipboard was busy or changed before file delivery");
                        report(Status::Ready);
                        remoteSequence = offer.sequence;
                        activeId.clear();
                        wake();
                    }
                }
            }
            catch (const std::exception&) {
                if (!state.stopping) qWarning("File clipboard transfer was cancelled or failed validation.");
                if (!state.stopping) report(Status::Failed);
                if (!activeId.empty() && !state.stopping) request(network, connection, QStringLiteral("cancel"), activeId, 0, UINT32_MAX, {}, true, stop);
            }
            std::unique_lock<std::mutex> lock(state.mutex);
            state.changed.wait_for(lock, std::chrono::milliseconds(350), [&] { return state.stopping || state.generation != generation; });
        }
    }
    Connection connection;
    std::filesystem::path storage;
    State state;
    std::shared_ptr<ClipboardCoordinator> coordinator;
    std::unique_ptr<QThread> thread;
    uint32_t observed = 0;
    int wakeCode;
};

ClipboardFileSync::ClipboardFileSync(const NvComputer* computer, int wakeEventCode, std::shared_ptr<ClipboardCoordinator> coordinator) : m_Impl(std::make_unique<Impl>(computer, wakeEventCode, std::move(coordinator))) {}
ClipboardFileSync::~ClipboardFileSync() = default;
void ClipboardFileSync::pump() {
    // All native clipboard, hashing and disk operations run on the worker.
    m_Impl->state.changed.notify_all();
}
void ClipboardFileSync::notifyLocalClipboardChange() { pump(); }
std::optional<ClipboardFileSync::Status> ClipboardFileSync::takeStatus() {
    std::lock_guard<std::mutex> lock(m_Impl->state.mutex);
    auto status = m_Impl->state.pendingStatus;
    m_Impl->state.pendingStatus.reset();
    return status;
}
#else
class ClipboardFileSync::Impl {};
ClipboardFileSync::ClipboardFileSync(const NvComputer*, int, std::shared_ptr<ClipboardCoordinator>) : m_Impl(std::make_unique<Impl>()) {}
ClipboardFileSync::~ClipboardFileSync() = default;
void ClipboardFileSync::pump() {}
void ClipboardFileSync::notifyLocalClipboardChange() {}
std::optional<ClipboardFileSync::Status> ClipboardFileSync::takeStatus() { return {}; }
#endif
