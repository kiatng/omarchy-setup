#include "live.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QtEndian>

// ---------------------------------------------------------------- Captions

QStringList CaptionAssembler::add(const QString &piece) {
    // The Live API sends each turn's transcript in pieces. Should a piece
    // ever repeat everything so far, it replaces rather than doubles it.
    if (!m_text.isEmpty() && piece.size() > m_text.size() && piece.startsWith(m_text))
        m_text = piece;
    else
        m_text += piece;

    QStringList done;
    while (current().size() > m_softLimit) {
        // Break after the last sentence end that leaves a real caption behind.
        static const QRegularExpression sentenceEnd(QStringLiteral("[.!?…෴।](?=\\s)"));
        int cut = -1;
        auto matches = sentenceEnd.globalMatch(m_text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (m_text.left(match.capturedEnd()).simplified().size() >= qMin(40, m_softLimit / 3))
                cut = match.capturedEnd();
        }
        // A long run with no sentence end at all breaks at a space.
        if (cut < 0 && current().size() > 2 * m_softLimit)
            cut = m_text.lastIndexOf(' ', m_text.size() - 2);
        if (cut <= 0)
            break;
        const QString head = m_text.left(cut).simplified();
        if (head.isEmpty())
            break;
        done << head;
        m_text = m_text.mid(cut);
    }
    return done;
}

QString CaptionAssembler::take() {
    const QString text = current();
    m_text.clear();
    return text;
}

// ---------------------------------------------------------------- Gemini

GeminiLiveProvider::GeminiLiveProvider(const QUrl &endpoint, QObject *parent)
    : StreamingProvider(parent), m_endpoint(endpoint) {
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &GeminiLiveProvider::connectSocket);
    m_finish.setSingleShot(true);
    m_finish.setInterval(4000);
    connect(&m_finish, &QTimer::timeout, this, [this] {
        commit();
        close();
    });
}

GeminiLiveProvider::~GeminiLiveProvider() {
    close();
}

QString GeminiLiveProvider::summary() const {
    return tr("Cloud, streaming: captions appear while people are still speaking. Audio streams to Google "
              "the whole time Relay is live.");
}

QList<ModelInfo> GeminiLiveProvider::models() const {
    return {{"gemini-3.5-live-translate-preview", tr("3.5 Live Translate (preview)"),
             tr("Lists Sinhala and Tamil. Free tier: no charge, but Google may use the audio to improve its "
                "products. Paid key: about US$2.20 per hour, and the audio isn't used that way."),
             {}}};
}

QByteArray GeminiLiveProvider::setupMessage(const QString &model, const QString &handle) {
    QJsonObject translation{{"targetLanguageCode", "en"},
                            // English speech gets English captions too, not silence.
                            {"echoTargetLanguage", true}};
    QJsonObject generation{{"responseModalities", QJsonArray{"AUDIO"}},
                           {"translationConfig", translation}};
    QJsonObject resumption;
    if (!handle.isEmpty())
        resumption.insert("handle", handle);
    QJsonObject setup{{"model", "models/" + model},
                      {"generationConfig", generation},
                      // The English text is the caption. The source transcript is
                      // asked for only for the language it names.
                      {"inputAudioTranscription", QJsonObject()},
                      {"outputAudioTranscription", QJsonObject()},
                      // Without compression an audio session ends after 15 minutes.
                      {"contextWindowCompression", QJsonObject{{"slidingWindow", QJsonObject()}}},
                      {"sessionResumption", resumption}};
    return QJsonDocument(QJsonObject{{"setup", setup}}).toJson(QJsonDocument::Compact);
}

QPair<QString, QString> GeminiLiveProvider::describeClose(int code, const QString &reason, const QString &key) {
    const QString detail = redact(reason.left(300), key).trimmed();
    const QString lower = detail.toLower();
    const auto with = [&](const QString &message) { return detail.isEmpty() ? message : message + " (" + detail + ")"; };

    if (lower.contains("api key") || lower.contains("api_key") || lower.contains("unauthenticated")
        || lower.contains("permission") || lower.contains("unregistered caller"))
        return {with(tr("%1 rejected the API key. Check it in Settings.").arg("Gemini")), "key"};
    if (lower.contains("quota") || lower.contains("resource_exhausted") || lower.contains("rate limit")
        || lower.contains("too many"))
        return {with(tr("Gemini says this key has reached its usage limit. Free-tier limits reset after a "
                        "while; try again later.")), "network"};
    if (lower.contains("not found") || lower.contains("not supported") || lower.contains("unknown name")
        || lower.contains("invalid argument") || lower.contains("invalid json"))
        return {with(tr("Gemini couldn't start a translation session.")), "model"};
    // 1007 and 1008 are the server refusing what was sent; asking again won't change that.
    if (code == 1007 || code == 1008)
        return {with(tr("Gemini ended the session.")), "model"};
    return {};
}

void GeminiLiveProvider::open(const QString &model, const QString &key) {
    close();
    m_model = model;
    m_key = key;
    m_open = true;
    m_attempts = 0;
    connectSocket();
}

void GeminiLiveProvider::connectSocket() {
    if (!m_open)
        return;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_ready = false;
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_socket = socket;
    auto lost = [this, socket] {
        if (socket != m_socket)
            return;
        m_socket = nullptr;
        socket->disconnect(this);
        socket->deleteLater();
        // Read the close before the socket goes.
        const int code = socket->closeCode();
        const QString reason = socket->closeReason().isEmpty() && socket->error() != QAbstractSocket::UnknownSocketError
            ? socket->errorString() : socket->closeReason();
        m_ready = false;
        emit readyChanged();
        if (!m_open)
            return;
        if (m_finishing) {
            commit();
            close();
            return;
        }
        const auto [message, action] = describeClose(code, reason, m_key);
        if (!message.isEmpty()) {
            commit();
            close();
            emit failed(message, action);
            return;
        }
        // A dropped connection resumes the session; one that won't come
        // back at all is reported.
        if (++m_attempts > 4) {
            commit();
            close();
            emit failed(tr("Can't reach %1. Check the network.").arg(name()), "network");
            return;
        }
        if (m_attempts > 2)
            m_handle.clear(); // a stale resumption handle may be the problem
        m_retry.start(m_retryDelayMs * (1 << (m_attempts - 1)));
    };
    connect(socket, &QWebSocket::connected, this, [this, socket] {
        if (socket == m_socket)
            socket->sendTextMessage(QString::fromUtf8(setupMessage(m_model, m_handle)));
    });
    // The Live API sends JSON in binary frames.
    connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &message) {
        if (socket == m_socket)
            handle(message.toUtf8());
    });
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket](const QByteArray &message) {
        if (socket == m_socket)
            handle(message);
    });
    connect(socket, &QWebSocket::disconnected, this, lost);
    connect(socket, &QWebSocket::errorOccurred, this, [socket, lost](QAbstractSocket::SocketError) {
        if (socket->state() == QAbstractSocket::UnconnectedState)
            QMetaObject::invokeMethod(socket, lost, Qt::QueuedConnection);
    });

    // The key goes in the URL as the Live API documents; the URL is never
    // shown or logged.
    QUrl url = m_endpoint;
    QUrlQuery query(url);
    query.addQueryItem("key", m_key);
    url.setQuery(query);
    socket->open(url);
}

void GeminiLiveProvider::handle(const QByteArray &message) {
    const QJsonObject object = QJsonDocument::fromJson(message).object();
    if (object.contains("setupComplete")) {
        m_ready = true;
        m_attempts = 0;
        emit readyChanged();
        if (m_finishing) {
            flushAudio(true);
            m_socket->sendTextMessage(R"({"realtimeInput":{"audioStreamEnd":true}})");
        } else {
            flushAudio(false);
        }
    }
    const QJsonObject resumption = object.value("sessionResumptionUpdate").toObject();
    if (resumption.value("resumable").toBool() && !resumption.value("newHandle").toString().isEmpty())
        m_handle = resumption.value("newHandle").toString();
    if (object.contains("goAway")) {
        // The server is about to end this connection; move to a new one now.
        commit();
        m_retry.start(0);
    }

    const QJsonObject content = object.value("serverContent").toObject();
    const QString heard = content.value("inputTranscription").toObject().value("languageCode").toString();
    if (!heard.isEmpty()) {
        const QString language = HttpProvider::languageName(heard);
        m_language = language == "English" ? QString() : language;
    }
    const QString english = content.value("outputTranscription").toObject().value("text").toString();
    if (!english.isEmpty()) {
        for (const QString &done : m_assembler.add(english))
            emit caption(done, m_language, true);
        if (!m_assembler.isEmpty())
            emit caption(m_assembler.current(), m_language, false);
    }
    if (content.value("generationComplete").toBool() || content.value("turnComplete").toBool()
        || content.value("interrupted").toBool())
        commit();
}

void GeminiLiveProvider::commit() {
    const QString text = m_assembler.take();
    if (!text.isEmpty())
        emit caption(text, m_language, true);
}

void GeminiLiveProvider::send(const QVector<qint16> &samples) {
    if (!m_open || m_finishing)
        return;
    m_pending += samples;
    if (m_ready) {
        flushAudio(false);
        return;
    }
    // While connecting, keep the last few seconds; older audio is dropped.
    const int keep = 16000 * 5;
    if (m_pending.size() > keep)
        m_pending.remove(0, m_pending.size() - keep);
}

void GeminiLiveProvider::flushAudio(bool all) {
    // 100 ms frames, as the Live API recommends.
    const int frame = 1600;
    int offset = 0;
    while (m_pending.size() - offset >= frame) {
        sendFrame(m_pending.mid(offset, frame));
        offset += frame;
    }
    if (all && offset < m_pending.size()) {
        sendFrame(m_pending.mid(offset));
        offset = int(m_pending.size());
    }
    m_pending.remove(0, offset);
}

void GeminiLiveProvider::sendFrame(const QVector<qint16> &frame) {
    if (!m_socket || !m_ready)
        return;
    QByteArray pcm(frame.size() * 2, Qt::Uninitialized);
    qToLittleEndian<qint16>(frame.constData(), frame.size(), pcm.data());
    const QJsonObject audio{{"data", QString::fromLatin1(pcm.toBase64())}, {"mimeType", "audio/pcm;rate=16000"}};
    m_socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(QJsonObject{{"realtimeInput", QJsonObject{{"audio", audio}}}}).toJson(QJsonDocument::Compact)));
}

void GeminiLiveProvider::finish() {
    if (!m_open || m_finishing)
        return;
    m_finishing = true;
    m_retry.stop();
    if (m_ready) {
        flushAudio(true);
        m_socket->sendTextMessage(R"({"realtimeInput":{"audioStreamEnd":true}})");
    } else if (!m_socket) {
        commit();
        close();
        return;
    }
    m_finish.start();
}

void GeminiLiveProvider::close() {
    const bool wasReady = m_ready;
    m_open = m_ready = m_finishing = false;
    m_retry.stop();
    m_finish.stop();
    m_pending.clear();
    m_handle.clear();
    m_key.clear();
    m_assembler.take();
    if (m_socket) {
        QWebSocket *socket = m_socket;
        m_socket = nullptr;
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    if (wasReady)
        emit readyChanged();
}
