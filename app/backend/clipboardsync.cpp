#include "clipboardsync.h"
#include "clipboardcoordinator.h"
#include "identitymanager.h"
#include "nvcomputer.h"
#include "SDL_compat.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QSslError>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace {
constexpr int MaximumTextBytes = 1024 * 1024;

ClipboardSync::Connection connectionFor(const NvComputer* computer)
{
    QUrl origin;
    origin.setScheme(QStringLiteral("https"));
    origin.setHost(computer->activeAddress.address());
    origin.setPort(computer->activeHttpsPort);
    auto* identity = IdentityManager::get();
    return { origin, computer->serverCert, identity->getSslConfig(), identity->getUniqueId() };
}

bool supportsClipboard(const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    QString version;
    int limit = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QStringLiteral("clipboardVersion")) {
                version = reader.readElementText();
            }
            else if (reader.name() == QStringLiteral("clipboardMaxBytes")) {
                limit = reader.readElementText().toInt();
            }
        }
    }
    return !reader.hasError() && version == QStringLiteral("1") && limit >= MaximumTextBytes;
}

struct ClipboardState {
    QMutex mutex;
    quint64 localGeneration = 0;
    struct Delivery { quint64 generation; QByteArray text; };
    std::optional<Delivery> delivery;
};

std::optional<QByteArray> localText()
{
#ifdef Q_OS_WIN
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        return std::nullopt;
    }
#endif
    if (!SDL_HasClipboardText()) {
#ifdef Q_OS_WIN
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) || CountClipboardFormats() == 0) {
            return QByteArray();
        }
#endif
        return std::nullopt; // Leave files, images and other clipboard formats alone.
    }
    char* text = SDL_GetClipboardText();
    if (!text) {
        return std::nullopt;
    }
    int length = 0;
    while (length <= MaximumTextBytes && text[length]) {
        length++;
    }
    std::optional<QByteArray> result;
    if (length <= MaximumTextBytes) {
        result = QByteArray(text, length);
    }
    SDL_free(text);
    return result;
}

std::optional<quint64> writeText(const QByteArray& text, quint64 expectedSequence)
{
#ifdef Q_OS_WIN
    const auto unicode = QString::fromUtf8(text);
    struct Allocation {
        HGLOBAL value = nullptr;
        ~Allocation() { if (value) GlobalFree(value); }
        bool prepare(const void* bytes, size_t length) {
            value = GlobalAlloc(GMEM_MOVEABLE, length);
            auto output = value ? GlobalLock(value) : nullptr;
            if (!output) return false;
            std::memcpy(output, bytes, length);
            GlobalUnlock(value);
            return true;
        }
        bool publish(UINT format) {
            if (!SetClipboardData(format, value)) return false;
            value = nullptr;
            return true;
        }
    } unicodeData, ansiData, oemData, localeData;
    LCID locale = MAKELCID(LOWORD(GetKeyboardLayout(0)), SORT_DEFAULT);
    DWORD ansiPage = 0, oemPage = 0;
    auto codePages = [&] {
        return GetLocaleInfoW(locale, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                   reinterpret_cast<LPWSTR>(&ansiPage), sizeof(ansiPage) / sizeof(wchar_t))
            && GetLocaleInfoW(locale, LOCALE_IDEFAULTCODEPAGE | LOCALE_RETURN_NUMBER,
                   reinterpret_cast<LPWSTR>(&oemPage), sizeof(oemPage) / sizeof(wchar_t));
    };
    if (!codePages()) {
        locale = GetUserDefaultLCID();
        if (!codePages()) return std::nullopt;
    }
    if (!ansiPage) ansiPage = GetACP();
    if (!oemPage) oemPage = GetOEMCP();
    const auto wide = reinterpret_cast<const wchar_t*>(unicode.utf16());
    auto encoded = [&](Allocation& allocation, UINT codePage) {
        const int size = WideCharToMultiByte(codePage, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (!size) return false;
        std::vector<char> bytes(size);
        return WideCharToMultiByte(codePage, 0, wide, -1, bytes.data(), size, nullptr, nullptr) == size
            && allocation.prepare(bytes.data(), bytes.size());
    };
    // CloseClipboard registers missing text/locale conversions and advances
    // the native generation. Prepare every format before taking the lock so
    // the captured generation stays correct through close without a race.
    if (!unicodeData.prepare(wide, (unicode.size() + 1) * sizeof(wchar_t))
        || !encoded(ansiData, ansiPage) || !encoded(oemData, oemPage)
        || !localeData.prepare(&locale, sizeof(locale))) return std::nullopt;
    // A real HWND owns this immediate-rendered format. No window registration,
    // message pumping, retry loop or long clipboard lock is needed here.
    struct OwnerWindow {
        HWND value;
        ~OwnerWindow() { if (value) DestroyWindow(value); }
    } owner {CreateWindowExW(0, L"STATIC", L"Moonlight clipboard", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr)};
    if (!owner.value || !OpenClipboard(owner.value)) return std::nullopt;
    struct ClipboardLock { ~ClipboardLock() { CloseClipboard(); } } clipboard;
    if (GetClipboardSequenceNumber() != expectedSequence || !EmptyClipboard()
        || !unicodeData.publish(CF_UNICODETEXT) || !ansiData.publish(CF_TEXT)
        || !oemData.publish(CF_OEMTEXT) || !localeData.publish(CF_LOCALE)) return std::nullopt;
    return static_cast<quint64>(GetClipboardSequenceNumber());
#else
    return SDL_SetClipboardText(text.constData()) == 0
        ? std::optional<quint64>(expectedSequence) : std::nullopt;
#endif
}

// No clipboard operations occur on this thread. All callbacks have a QObject
// context, so destroying the worker cancels timers and queued submissions.
class ClipboardWorker : public QObject {
public:
    ClipboardWorker(QUrl origin, QSslCertificate certificate, QSslConfiguration identity, QString uniqueId,
                    ClipboardState& state, int wakeCode, std::shared_ptr<ClipboardCoordinator> coordinator) :
        m_Origin(origin), m_Certificate(certificate), m_Identity(identity),
        m_UniqueId(uniqueId), m_State(state), m_WakeCode(wakeCode), m_Coordinator(std::move(coordinator))
    {}

    void start()
    {
        m_Network = new QNetworkAccessManager(this);
        m_Network->setProxy(QNetworkProxy::NoProxy);
        auto* timer = new QTimer(this);
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start();
        tick();
    }

    void submit(std::optional<QByteArray> text, quint64 generation, uint32_t sequence)
    {
        if (!currentGeneration(generation)) {
            return;
        }
        if (text) {
            m_PendingLocal = PendingLocal {generation, sequence, std::move(*text)};
        }
        else {
            m_PendingLocal.reset();
            if (!m_HostObserved) m_DiscardInitialHost = true;
        }
        // A newer text copy waits for an existing POST's returned host cursor;
        // aborting it after the host commits would make our next copy conflict
        // with our own mutation. Non-text copies cancel the older text request.
        // Let an existing GET finish to consume its host sequence:
        // its generation check discards the stale body without re-fetching it
        // as a new copy on the next tick.
        if (!text && m_ActiveClipboardReply && m_ActiveClipboardIsPost) {
            m_ActiveClipboardReply->abort();
        }
        if (m_Enabled) {
            tick();
        }
    }

private:
    using Completion = std::function<void(int, QByteArray)>;

    bool currentGeneration(quint64 generation)
    {
        QMutexLocker lock(&m_State.mutex);
        return generation == m_State.localGeneration;
    }

    void wake()
    {
        SDL_Event event = {};
        event.type = SDL_USEREVENT;
        event.user.code = m_WakeCode;
        SDL_PushEvent(&event);
    }

    void request(const QString& path, const QByteArray& body, bool post, Completion completion)
    {
        QUrl url(m_Origin);
        url.setPath(path);
        QUrlQuery query;
        if (path == QStringLiteral("/serverinfo")) {
            query.addQueryItem(QStringLiteral("uniqueid"), m_UniqueId);
        }
        else {
            query.addQueryItem(QStringLiteral("type"), QStringLiteral("text"));
            if (!m_Sequence.isEmpty()) {
                query.addQueryItem(QStringLiteral("since"), QString::fromLatin1(m_Sequence));
            }
        }
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setSslConfiguration(m_Identity);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("Cache-Control", "no-store");
        if (post) {
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain; charset=utf-8"));
        }
        auto* reply = post ? m_Network->post(request, body) : m_Network->get(request);
        if (path == QStringLiteral("/actions/clipboard")) {
            m_ActiveClipboardReply = reply;
            m_ActiveClipboardIsPost = post;
        }
        reply->setReadBufferSize(MaximumTextBytes + 1);
        auto data = std::make_shared<QByteArray>();
        connect(reply, &QNetworkReply::sslErrors, reply, [this, reply](const QList<QSslError>& errors) {
            for (const auto& error : errors) {
                if (error.certificate() != m_Certificate) {
                    return;
                }
            }
            reply->ignoreSslErrors(errors); // Trust only the exact previously paired host.
        });
        connect(reply, &QNetworkReply::encrypted, reply, [this, reply] {
            if (reply->sslConfiguration().peerCertificate() != m_Certificate) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::readyRead, reply, [reply, data] {
            *data += reply->readAll();
            if (data->size() > MaximumTextBytes) {
                reply->abort();
            }
        });
        QTimer::singleShot(3000, reply, [reply] {
            if (!reply->isFinished()) {
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, data, completion, path, post] {
            if (m_ActiveClipboardReply == reply) {
                m_ActiveClipboardReply.clear();
            }
            *data += reply->readAll();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool trusted = reply->sslConfiguration().peerCertificate() == m_Certificate;
            const bool success = trusted && reply->error() == QNetworkReply::NoError
                && data->size() <= MaximumTextBytes;
            if (success && path == QStringLiteral("/actions/clipboard")) {
                m_Sequence = reply->rawHeader("X-Sunshine-Clipboard-Sequence");
                if (success && !post && (status == 200 || status == 204)) {
                    m_HostObserved = true;
                }
            }
            completion(success ? status : (status >= 400 ? status : 0), success ? *data : QByteArray());
            reply->deleteLater();
        });
    }

    void tick()
    {
        wake(); // SDL's blocking loop must sample local clipboard changes, too.
        if (m_Disabled || m_Busy) {
            return;
        }
        m_Busy = true;
        if (!m_Enabled) {
            request(QStringLiteral("/serverinfo"), {}, false, [this](int status, QByteArray xml) {
                m_Busy = false;
                if (status == 200) {
                    m_Enabled = supportsClipboard(xml);
                    m_Disabled = !m_Enabled;
                    if (m_Disabled) {
                        if (m_Coordinator) m_Coordinator->disableText();
                        qInfo("The host does not advertise text clipboard sync. Enable it in Sunshine first.");
                    }
                }
            });
            return;
        }
        // Establish the host cursor before the first mutation. A newer host
        // copy after this observation can then reject the stale POST.
        if (m_PendingLocal && m_HostObserved) {
            const quint64 generation = m_PendingLocal->generation;
            const uint32_t sequence = m_PendingLocal->sequence;
            const QByteArray text = std::move(m_PendingLocal->text);
            m_PendingLocal.reset();
            if (!currentGeneration(generation) || (m_Coordinator && !m_Coordinator->textPending(sequence))) {
                m_Busy = false;
                return;
            }
            request(QStringLiteral("/actions/clipboard"), text, true, [this, text, generation, sequence](int status, QByteArray) {
                m_Busy = false;
                if (status == 200) {
                    m_LastRemote = text;
                    m_LastRemoteSequence = m_Sequence;
                    if (m_Coordinator) {
                        bool valid = false;
                        const uint32_t hostSequence = m_Sequence.toUInt(&valid);
                        m_Coordinator->settleText(sequence, valid ? std::optional<uint32_t>(hostSequence) : std::nullopt);
                    }
                }
                else if (status == 403 || status == 404) {
                    m_Disabled = true;
                    if (m_Coordinator) m_Coordinator->disableText();
                    qWarning("Text clipboard sync was disabled by the host (HTTP %d).", status);
                }
                else if (status == 409) {
                    // The host was copied again since our last observation.
                    // Discard this superseded text rather than retrying it over
                    // the new host selection. Observe that selection before
                    // delivering it or attempting a newer pending mutation.
                    m_HostObserved = false;
                    if (m_Coordinator) m_Coordinator->settleText(sequence, {}, true);
                }
                else if (!m_PendingLocal && currentGeneration(generation)) {
                    m_PendingLocal = PendingLocal {generation, sequence, text};
                }
                else {
                    // A superseded request can have committed even when its
                    // reply fails. Re-observe before posting the newer text.
                    m_HostObserved = false;
                }
            });
            return;
        }
        quint64 generation;
        {
            QMutexLocker lock(&m_State.mutex);
            generation = m_State.localGeneration;
        }
        request(QStringLiteral("/actions/clipboard"), {}, false, [this, generation](int status, QByteArray text) {
            m_Busy = false;
            if (status == 403 || status == 404) {
                m_Disabled = true;
                if (m_Coordinator) m_Coordinator->disableText();
                qWarning("Text clipboard sync was disabled by the host (HTTP %d).", status);
                return;
            }
            if (status == 204) {
                m_DiscardInitialHost = false;
                return;
            }
            if (status != 200 || text.contains('\0') || QString::fromUtf8(text).toUtf8() != text) {
                return;
            }
            if (m_DiscardInitialHost) {
                // A file/image copy made before the first host observation
                // must survive startup. Consume the baseline without applying
                // its body, including content-only hosts lacking a cursor.
                m_DiscardInitialHost = false;
                m_LastRemote = text;
                m_LastRemoteSequence = m_Sequence;
                return;
            }
            QMutexLocker lock(&m_State.mutex);
            // A response started before a new local copy must not overwrite it.
            const bool alreadyObserved = m_LastRemote == text
                && (m_Sequence.isEmpty() || m_LastRemoteSequence == m_Sequence);
            if (generation != m_State.localGeneration || m_PendingLocal || alreadyObserved) {
                return;
            }
            m_LastRemote = text;
            m_LastRemoteSequence = m_Sequence;
            m_State.delivery = ClipboardState::Delivery { generation, text };
            lock.unlock();
            wake();
        });
    }

    QUrl m_Origin;
    QSslCertificate m_Certificate;
    QSslConfiguration m_Identity;
    QString m_UniqueId;
    ClipboardState& m_State;
    int m_WakeCode;
    QNetworkAccessManager* m_Network = nullptr;
    bool m_Busy = false;
    bool m_Enabled = false;
    bool m_HostObserved = false;
    bool m_DiscardInitialHost = false;
    bool m_Disabled = false;
    struct PendingLocal { quint64 generation; uint32_t sequence; QByteArray text; };
    std::optional<PendingLocal> m_PendingLocal;
    QPointer<QNetworkReply> m_ActiveClipboardReply;
    bool m_ActiveClipboardIsPost = false;
    std::optional<QByteArray> m_LastRemote;
    QByteArray m_LastRemoteSequence;
    QByteArray m_Sequence;
    std::shared_ptr<ClipboardCoordinator> m_Coordinator;
};
} // namespace

class ClipboardSync::Impl {
public:
    Impl(Connection connection, int wakeCode, ReadClipboard readClipboard, WriteClipboard writeClipboard,
         ReadSequence readSequence, std::shared_ptr<ClipboardCoordinator> clipboardCoordinator) :
        read(std::move(readClipboard)), write(std::move(writeClipboard)), sequence(std::move(readSequence)),
        observedSequence(sequence ? sequence() : 0), coordinator(std::move(clipboardCoordinator))
    {
        observed = read();
        // Never associate text from before an intervening file/native copy
        // with that newer copy's sequence, including during stream startup.
        if (sequence && sequence() != observedSequence) forceSample = true;
        worker = new ClipboardWorker(connection.origin, connection.pinnedCertificate,
            connection.clientIdentity, connection.uniqueId, state, wakeCode, coordinator);
        if (coordinator) coordinator->initialize(static_cast<uint32_t>(observedSequence));
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::started, worker, [this] { worker->start(); });
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }

    ~Impl()
    {
        thread.quit();
        thread.wait();
    }

    ClipboardState state;
    QThread thread;
    ClipboardWorker* worker;
    ReadClipboard read;
    WriteClipboard write;
    ReadSequence sequence;
    std::optional<QByteArray> observed;
    quint64 observedSequence;
    std::shared_ptr<ClipboardCoordinator> coordinator;
    bool forceSample = false;
    std::chrono::steady_clock::time_point lastSample;
    std::optional<ClipboardState::Delivery> retryDelivery;
    std::chrono::steady_clock::time_point retryAfter;
};

ClipboardSync::ClipboardSync(const NvComputer* computer, int wakeEventCode, std::shared_ptr<ClipboardCoordinator> coordinator) :
    ClipboardSync(connectionFor(computer), wakeEventCode, localText, writeText,
#ifdef Q_OS_WIN
        [] { return static_cast<quint64>(GetClipboardSequenceNumber()); }, std::move(coordinator))
#else
        {}, std::move(coordinator))
#endif
{}

ClipboardSync::ClipboardSync(Connection connection, int wakeEventCode, ReadClipboard read, WriteClipboard write,
                             ReadSequence sequence, std::shared_ptr<ClipboardCoordinator> coordinator) :
    m_Impl(std::make_unique<Impl>(std::move(connection), wakeEventCode, std::move(read), std::move(write), std::move(sequence), std::move(coordinator)))
{}

ClipboardSync::~ClipboardSync() = default;

void ClipboardSync::pump()
{
    const auto now = std::chrono::steady_clock::now();
    std::optional<ClipboardState::Delivery> delivery;
    {
        QMutexLocker lock(&m_Impl->state.mutex);
        delivery = std::move(m_Impl->state.delivery);
        m_Impl->state.delivery.reset();
    }
    if (delivery) {
        m_Impl->retryDelivery.reset();
    }
    else if (m_Impl->retryDelivery && now >= m_Impl->retryAfter) {
        delivery = std::move(m_Impl->retryDelivery);
        m_Impl->retryDelivery.reset();
    }
    // A pending remote result always gets a fresh local observation. Sampling
    // only on a timer could overwrite a copy made within the last 200 ms.
    if (delivery || m_Impl->forceSample || now - m_Impl->lastSample >= std::chrono::milliseconds(200)) {
        m_Impl->lastSample = now;
        const quint64 beforeSequence = m_Impl->sequence ? m_Impl->sequence() : 0;
        auto current = m_Impl->read();
        const quint64 sequence = m_Impl->sequence ? m_Impl->sequence() : 0;
        if (beforeSequence != sequence) {
            // A worker or another app copied while reading. Leave both pending
            // local data and any remote delivery alone until a coherent sample.
            m_Impl->forceSample = true;
            if (delivery) {
                m_Impl->retryDelivery = std::move(delivery);
                m_Impl->retryAfter = now + std::chrono::milliseconds(250);
            }
            return;
        }
        bool resample = false;
        if (current != m_Impl->observed || sequence != m_Impl->observedSequence || (m_Impl->forceSample && !current)) {
            m_Impl->observed = current;
            m_Impl->observedSequence = sequence;
            const bool accepted = !m_Impl->coordinator
                || m_Impl->coordinator->observeLocal(static_cast<uint32_t>(sequence), current.has_value(), true);
            resample = m_Impl->sequence && m_Impl->sequence() != sequence;
            // A file publication can win the coordinator lock after sampling.
            // Its stale/remote observation cancels older text, never uploads
            // the rejected snapshot or echoes our own file publication.
            if (!accepted || resample) current.reset();
            quint64 generation;
            {
                QMutexLocker lock(&m_Impl->state.mutex);
                generation = ++m_Impl->state.localGeneration;
            }
            QMetaObject::invokeMethod(m_Impl->worker, [worker = m_Impl->worker, text = std::move(current), generation, sequence]() mutable {
                worker->submit(std::move(text), generation, static_cast<uint32_t>(sequence));
            }, Qt::QueuedConnection);
        }
        m_Impl->forceSample = resample;
    }
    {
        QMutexLocker lock(&m_Impl->state.mutex);
        if (delivery && delivery->generation != m_Impl->state.localGeneration) {
            delivery.reset();
        }
    }
    if (delivery) {
        // Recording this value suppresses the corresponding local clipboard echo.
        if (auto publishedSequence = m_Impl->write(delivery->text, m_Impl->observedSequence)) {
            m_Impl->observed = delivery->text;
            m_Impl->observedSequence = *publishedSequence;
            if (m_Impl->coordinator) m_Impl->coordinator->observeRemote(static_cast<uint32_t>(m_Impl->observedSequence));
        }
        else {
            m_Impl->forceSample = true;
            m_Impl->retryDelivery = std::move(delivery);
            m_Impl->retryAfter = now + std::chrono::milliseconds(250);
        }
    }
}

void ClipboardSync::notifyLocalClipboardChange()
{
    m_Impl->forceSample = true;
    pump();
}
