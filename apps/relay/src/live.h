#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include "providers.h"

// A provider that translates a continuous stream instead of phrases: audio
// goes out as the microphone delivers it, and English comes back while the
// person is still speaking. translate() is unused.
class StreamingProvider : public Provider {
    Q_OBJECT
public:
    using Provider::Provider;

    bool streaming() const override { return true; }
    void translate(const TranslationRequest &, const QString &, const QString &) override {}
    void cancelAll() override { close(); }

    // Starts a session. Audio sent before it's ready waits, a few seconds' worth at most.
    virtual void open(const QString &model, const QString &key) = 0;
    virtual void send(const QVector<qint16> &samples) = 0;   // 16 kHz mono
    // No more audio: let the last words come back, then close.
    virtual void finish() = 0;
    // Close now; nothing more arrives.
    virtual void close() = 0;
    virtual bool ready() const = 0;

signals:
    // The caption being spoken, in full so far; done when it won't change again.
    void caption(const QString &text, const QString &language, bool done);
    // The session can't go on. action is "key", "model", or "network".
    void failed(const QString &message, const QString &action);
    void readyChanged();
};

// Pieces of streamed English to captions of a readable length: a caption
// ends when the speaker's turn does, or at a sentence end once it's long.
class CaptionAssembler {
public:
    explicit CaptionAssembler(int softLimit = 160) : m_softLimit(softLimit) {}

    // Adds streamed text and returns any captions it completes.
    QStringList add(const QString &piece);
    // The caption so far, and nothing more to come for it.
    QString take();
    QString current() const { return m_text.simplified(); }
    bool isEmpty() const { return current().isEmpty(); }

private:
    QString m_text;
    int m_softLimit;
};

// Google's Gemini Live Translate over the Live API's WebSocket.
class GeminiLiveProvider : public StreamingProvider {
    Q_OBJECT
public:
    explicit GeminiLiveProvider(const QUrl &endpoint = QUrl(
        "wss://generativelanguage.googleapis.com/ws/"
        "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"),
        QObject *parent = nullptr);
    ~GeminiLiveProvider() override;

    QString id() const override { return "gemini"; }
    QString name() const override { return "Gemini"; }
    QString summary() const override;
    QList<ModelInfo> models() const override;

    void open(const QString &model, const QString &key) override;
    void send(const QVector<qint16> &samples) override;
    void finish() override;
    void close() override;
    bool ready() const override { return m_ready; }

    void setRetryDelayMs(int ms) { m_retryDelayMs = ms; }
    void setFinishGraceMs(int ms) { m_finish.setInterval(ms); }

    // The first message of a session, resuming one when handle is set.
    static QByteArray setupMessage(const QString &model, const QString &handle);
    // Why a closed session can't go on, and what would fix it; empty
    // message when reconnecting might help.
    static QPair<QString, QString> describeClose(int code, const QString &reason, const QString &key);

private:
    void connectSocket();
    void handle(const QByteArray &message);
    void flushAudio(bool all);
    void sendFrame(const QVector<qint16> &frame);
    void dropped();
    void commit();

    QUrl m_endpoint;
    QWebSocket *m_socket = nullptr;
    QString m_model, m_key, m_handle;
    QString m_language;
    bool m_open = false, m_ready = false, m_finishing = false;
    int m_attempts = 0;
    int m_retryDelayMs = 500;
    QVector<qint16> m_pending;       // audio not yet sent, bounded
    QTimer m_retry, m_finish;
    CaptionAssembler m_assembler;
};
