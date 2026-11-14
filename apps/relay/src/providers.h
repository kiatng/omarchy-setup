#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QUrl>
#include <QVector>

struct TranslationRequest {
    quint64 sequence = 0;
    QVector<qint16> samples; // 16 kHz mono
    QString context;         // the previous English caption, for continuity
};

struct Translation {
    quint64 sequence = 0;
    bool ok = false;
    bool fatal = false;     // retrying won't help (rejected key, missing model)
    bool keyRejected = false;
    QString text;           // English; empty when the chunk held no words
    QString language;       // detected source language, when the provider says
    QString error;          // for the window; never contains a key
    int latencyMs = 0;
    double audioSeconds = 0;
    double costUsd = 0;     // estimate, from the provider's published prices
};

Q_DECLARE_METATYPE(Translation)

struct ModelInfo {
    QString id;
    QString name;
    QString note;           // speed, price, and quality in a phrase
    QString problem;        // why it can't be used now; empty when it can
};

// One way of turning a chunk of speech into English. Each request finishes
// exactly once with finished(), unless cancelAll() drops it first.
class Provider : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString summary() const = 0;
    virtual bool needsKey() const { return true; }
    virtual bool local() const { return false; }
    virtual QList<ModelInfo> models() const = 0;
    virtual int maxConcurrent() const { return 2; }
    // Streams audio continuously instead of translating phrases; see live.h.
    virtual bool streaming() const { return false; }
    virtual void translate(const TranslationRequest &request, const QString &model, const QString &key) = 0;
    virtual void cancelAll() = 0;

    ModelInfo model(const QString &id) const;
    QString defaultModel() const;

    // Removes anything that could be a key from text shown or logged:
    // the key itself and anything shaped like a provider key.
    static QString redact(QString text, const QString &key = {});
    // Drops captions Whisper is known to invent from noise or silence.
    static QString dropHallucination(const QString &text);

signals:
    void finished(const Translation &result);
};

// Groq and OpenAI speak the same multipart audio API.
class HttpProvider : public Provider {
    Q_OBJECT
public:
    explicit HttpProvider(const QUrl &baseUrl, QObject *parent = nullptr);

    void cancelAll() override;
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

    // A Whisper JSON or verbose_json body to English text. Segments the model
    // itself flags as probably not speech, or as looping, are left out.
    static Translation parseWhisper(const QByteArray &body);
    // A failed reply to one sentence the user can act on.
    static QString describeError(const QString &provider, int status, QNetworkReply::NetworkError error,
                                 const QByteArray &body, const QString &key);
    static bool isFatal(int status);
    // "si", "sinhala" or "Sinhala" to "Sinhala".
    static QString languageName(const QString &codeOrName);

protected:
    using Done = std::function<void(int status, QNetworkReply::NetworkError error, const QByteArray &body)>;
    void postAudio(const QString &path, const QString &key, const QList<QPair<QByteArray, QByteArray>> &fields,
                   const QByteArray &wav, Done done);
    void postJson(const QString &path, const QString &key, const QJsonObject &body, Done done);
    void finish(Translation result, const QElapsedTimer &timer);

    QUrl m_baseUrl;
    int m_timeoutMs = 30000;
    QNetworkAccessManager m_network;
    QList<QPointer<QNetworkReply>> m_replies;
    quint64 m_generation = 0;

private:
    void track(QNetworkReply *reply, Done done);
};

class GroqProvider : public HttpProvider {
    Q_OBJECT
public:
    explicit GroqProvider(const QUrl &baseUrl = QUrl("https://api.groq.com/openai/v1"), QObject *parent = nullptr)
        : HttpProvider(baseUrl, parent) {}

    QString id() const override { return "groq"; }
    QString name() const override { return "Groq"; }
    QString summary() const override;
    QList<ModelInfo> models() const override;
    void translate(const TranslationRequest &request, const QString &model, const QString &key) override;

    // Groq bills at least 10 seconds for every request.
    static double cost(double seconds, const QString &model);
};

class OpenAIProvider : public HttpProvider {
    Q_OBJECT
public:
    explicit OpenAIProvider(const QUrl &baseUrl = QUrl("https://api.openai.com/v1"), QObject *parent = nullptr)
        : HttpProvider(baseUrl, parent) {}

    QString id() const override { return "openai"; }
    QString name() const override { return "OpenAI"; }
    QString summary() const override;
    QList<ModelInfo> models() const override;
    void translate(const TranslationRequest &request, const QString &model, const QString &key) override;

    static double cost(double seconds, const QString &model);
    // The text model's JSON reply ({"language", "english"}) to a result.
    static Translation parseTranslationReply(const QByteArray &body);

private:
    void translateText(const QString &original, const QString &heardLanguage, const TranslationRequest &request,
                       const QString &key, const QElapsedTimer &timer);
};

// Whisper on this machine through Voxtype's one-shot `transcribe` command.
// Nothing leaves the laptop, but only multilingual models translate, and the
// larger ones are far slower than speech on a laptop CPU.
class VoxtypeProvider : public Provider {
    Q_OBJECT
public:
    explicit VoxtypeProvider(const QString &program = {}, const QString &modelDirectory = {},
                             QObject *parent = nullptr);
    ~VoxtypeProvider() override;

    QString id() const override { return "voxtype"; }
    QString name() const override { return "Voxtype"; }
    QString summary() const override;
    bool needsKey() const override { return false; }
    bool local() const override { return true; }
    QList<ModelInfo> models() const override;
    int maxConcurrent() const override { return 1; }
    void translate(const TranslationRequest &request, const QString &model, const QString &key) override;
    void cancelAll() override;
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

    // Voxtype prints progress lines before the text on stdout.
    static QString parseOutput(const QByteArray &stdoutBytes);
    QString program() const { return m_program; }

private:
    QString m_program;
    QString m_modelDirectory;
    QString m_workDirectory;
    int m_timeoutMs = 120000;
    QList<QPointer<QProcess>> m_processes;
};
