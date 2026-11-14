#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMediaDevices>
#include <QObject>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

#include "captions.h"
#include "live.h"
#include "microphone.h"
#include "pipeline.h"
#include "providers.h"
#include "secrets.h"
#include "segmenter.h"

// Relay's one job: while live, hear the room, cut the speech into phrases,
// send each to the chosen provider, and show the English in order. The
// microphone is open only while live; audio and captions are kept in memory
// only; keys live in memory or the desktop secret store, never in settings.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool live READ live NOTIFY stateChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(QString problemAction READ problemAction NOTIFY problemChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceChanged)
    Q_PROPERTY(QVariantList providers READ providers CONSTANT)
    Q_PROPERTY(QVariantMap keyStates READ keyStates NOTIFY keyChanged)
    Q_PROPERTY(QString providerId READ providerId WRITE setProviderId NOTIFY providerChanged)
    Q_PROPERTY(QString providerName READ providerName NOTIFY providerChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY providerChanged)
    Q_PROPERTY(QString modelId READ modelId WRITE setModelId NOTIFY providerChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY providerChanged)
    Q_PROPERTY(bool needsKey READ needsKey NOTIFY providerChanged)
    Q_PROPERTY(QString keyState READ keyState NOTIFY keyChanged)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable CONSTANT)
    Q_PROPERTY(double level READ level NOTIFY levelChanged)
    Q_PROPERTY(bool hearing READ hearing NOTIFY levelChanged)
    Q_PROPERTY(int pending READ pending NOTIFY activityChanged)
    Q_PROPERTY(bool connecting READ connecting NOTIFY activityChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY providerChanged)
    Q_PROPERTY(int lastLatencyMs READ lastLatencyMs NOTIFY statsChanged)
    Q_PROPERTY(double sessionCost READ sessionCost NOTIFY statsChanged)
    Q_PROPERTY(double audioSeconds READ audioSeconds NOTIFY statsChanged)
    Q_PROPERTY(int chunkSeconds READ chunkSeconds WRITE setChunkSeconds NOTIFY settingsChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY settingsChanged)
    Q_PROPERTY(QObject *captions READ captions CONSTANT)

public:
    struct Options {
        QString settingsPath;                  // empty: ~/.config/relay/relay.conf
        QString stateFile;                     // empty: $XDG_RUNTIME_DIR/relay/state
        QList<Provider *> providers;           // empty: Gemini, Groq, OpenAI, Voxtype
        Secrets *secrets = nullptr;            // empty: secret-tool
        Microphone *microphone = nullptr;      // empty: the real one
        bool listDevices = true;
    };

    explicit Backend(QObject *parent = nullptr) : Backend(Options{}, parent) {}
    explicit Backend(const Options &options, QObject *parent = nullptr);
    ~Backend() override;

    QString state() const { return m_state; }
    bool live() const { return m_state == "live"; }
    QString problem() const { return m_problem; }
    QString problemAction() const { return m_problemAction; }
    QVariantList devices() const;
    QString deviceId() const { return m_deviceId; }
    void setDeviceId(const QString &id);
    QVariantList providers() const;
    QString providerId() const;
    QString providerName() const;
    void setProviderId(const QString &id);
    QVariantList models() const;
    QString modelId() const { return m_model; }
    QString modelName() const;
    void setModelId(const QString &id);
    bool needsKey() const;
    QString keyState() const { return keyStateFor(provider()->id()); }
    // "none" (no key needed), "missing", "checking", "session", or "saved".
    QString keyStateFor(const QString &id) const;
    QVariantMap keyStates() const;  // provider id to keyStateFor()
    // Where to create a key for a provider.
    static QString keyUrl(const QString &id);
    bool keyringAvailable() const { return m_secrets->available(); }
    double level() const { return m_level; }
    bool hearing() const { return m_segmenter.inSpeech(); }
    int pending() const { return m_pipeline.inFlight() + m_pipeline.waiting(); }
    bool streaming() const { return provider()->streaming(); }
    // Live, but the streaming session isn't up yet.
    bool connecting() const;
    int lastLatencyMs() const { return m_lastLatencyMs; }
    double sessionCost() const { return m_cost; }
    double audioSeconds() const { return m_audioSeconds; }
    int chunkSeconds() const { return m_chunkSeconds; }
    void setChunkSeconds(int seconds);
    int fontSize() const { return m_fontSize; }
    void setFontSize(int size);
    QObject *captions() { return &m_captions; }
    Captions &captionList() { return m_captions; }

    Q_INVOKABLE bool start();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void clearCaptions();
    Q_INVOKABLE void copyCaptions();
    Q_INVOKABLE void setKey(const QString &key, bool remember) { setKeyFor(providerId(), key, remember); }
    Q_INVOKABLE void forgetKey() { forgetKeyFor(providerId()); }
    Q_INVOKABLE void setKeyFor(const QString &provider, const QString &key, bool remember);
    Q_INVOKABLE void forgetKeyFor(const QString &provider);
    Q_INVOKABLE void refreshModels() { emit providerChanged(); }
    Q_INVOKABLE void dismissProblem() { setProblem({}, {}); }
    // Stops listening and drops everything in flight; nothing more is sent.
    Q_INVOKABLE void shutdown();

    // Samples as the microphone delivers them; public for tests.
    void hear(const QVector<qint16> &samples);

signals:
    void stateChanged();
    void problemChanged();
    void devicesChanged();
    void deviceChanged();
    void providerChanged();
    void keyChanged();
    void levelChanged();
    void activityChanged();
    void statsChanged();
    void settingsChanged();
    void copied();

private:
    Provider *provider() const;
    StreamingProvider *stream() const { return qobject_cast<StreamingProvider *>(provider()); }
    void streamed(const QString &text, const QString &language, bool done);
    void streamFailed(const QString &message, const QString &action);
    void setState(const QString &state);
    void setProblem(const QString &text, const QString &action);
    void received(const Translation &translation);
    void loadKey(const QString &provider);
    void applySegmenter();
    void writeStateFile();

    std::unique_ptr<QSettings> m_settings;
    QString m_stateFile;
    QList<Provider *> m_providers;
    Secrets *m_secrets = nullptr;
    Microphone *m_microphone = nullptr;
    QMediaDevices *m_mediaDevices = nullptr;
    Pipeline m_pipeline;
    Segmenter m_segmenter;
    Captions m_captions;

    QString m_state = "paused";
    QString m_problem, m_problemAction;
    QString m_deviceId;
    int m_providerIndex = 0;
    QString m_model;
    QHash<QString, QString> m_keys;       // in memory only
    QHash<QString, QString> m_keySource;  // "session", "saved", "checking"
    double m_level = 0;
    QElapsedTimer m_levelTimer, m_statsTimer;
    int m_lastLatencyMs = 0;
    double m_cost = 0, m_audioSeconds = 0;
    int m_chunkSeconds = 8;
    int m_fontSize = 40;
    int m_failuresInRow = 0;
};
