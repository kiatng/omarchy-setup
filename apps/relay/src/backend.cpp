#include "backend.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QStandardPaths>

Backend::Backend(const Options &options, QObject *parent) : QObject(parent) {
    const QString settingsPath = !options.settingsPath.isEmpty()
        ? options.settingsPath
        : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/relay/relay.conf";
    m_settings = std::make_unique<QSettings>(settingsPath, QSettings::IniFormat);

    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    m_stateFile = !options.stateFile.isEmpty() ? options.stateFile
                                               : (runtime.isEmpty() ? QString() : runtime + "/relay/state");

    m_providers = options.providers;
    if (m_providers.isEmpty())
        m_providers = {new GeminiLiveProvider(), new GroqProvider(), new OpenAIProvider(), new VoxtypeProvider()};
    for (Provider *p : m_providers) {
        p->setParent(this);
        if (auto *s = qobject_cast<StreamingProvider *>(p)) {
            connect(s, &StreamingProvider::caption, this, [this, s](const QString &text, const QString &language, bool done) {
                if (s == stream())
                    streamed(text, language, done);
            });
            connect(s, &StreamingProvider::failed, this, [this, s](const QString &message, const QString &action) {
                if (s == stream())
                    streamFailed(message, action);
            });
            connect(s, &StreamingProvider::readyChanged, this, &Backend::activityChanged);
        }
    }
    m_secrets = options.secrets ? options.secrets : new Secrets();
    m_secrets->setParent(this);
    m_microphone = options.microphone ? options.microphone : new Microphone();
    m_microphone->setParent(this);

    connect(m_microphone, &Microphone::samples, this, &Backend::hear);
    connect(m_microphone, &Microphone::failed, this, [this](const QString &message) {
        pause();
        setProblem(message, "device");
    });
    connect(&m_pipeline, &Pipeline::result, this, &Backend::received);
    connect(&m_pipeline, &Pipeline::activityChanged, this, &Backend::activityChanged);
    connect(&m_pipeline, &Pipeline::skipped, this, [this](double seconds) {
        m_captions.append(Captions::Notice,
                          tr("Relay fell behind and skipped %1 seconds of speech.")
                              .arg(QLocale().toString(seconds, 'f', 0)));
    });

    if (options.listDevices) {
        m_mediaDevices = new QMediaDevices(this);
        connect(m_mediaDevices, &QMediaDevices::audioInputsChanged, this, &Backend::devicesChanged);
    }

    // Settings hold choices only: never a key, audio, or caption.
    m_deviceId = m_settings->value("microphone").toString();
    const QString savedProvider = m_settings->value("provider", m_providers.first()->id()).toString();
    for (int i = 0; i < m_providers.size(); ++i)
        if (m_providers.at(i)->id() == savedProvider)
            m_providerIndex = i;
    m_model = m_settings->value("model/" + provider()->id(), provider()->defaultModel()).toString();
    if (provider()->model(m_model).id.isEmpty())
        m_model = provider()->defaultModel();
    m_chunkSeconds = qBound(4, m_settings->value("chunkSeconds", 8).toInt(), 15);
    m_fontSize = qBound(20, m_settings->value("fontSize", 40).toInt(), 96);
    applySegmenter();
    // Every provider's saved key, so Settings can show which have one.
    for (Provider *p : m_providers)
        loadKey(p->id());
    writeStateFile();
}

Backend::~Backend() {
    shutdown();
    if (!m_stateFile.isEmpty())
        QFile::remove(m_stateFile);
}

Provider *Backend::provider() const {
    return m_providers.at(m_providerIndex);
}

bool Backend::connecting() const {
    return live() && stream() && !stream()->ready();
}

QVariantList Backend::devices() const {
    QVariantList list;
    list << QVariantMap{{"id", QString()}, {"name", tr("System default")}};
    for (const auto &device : QMediaDevices::audioInputs())
        list << QVariantMap{{"id", QString::fromUtf8(device.id())}, {"name", device.description()}};
    return list;
}

void Backend::setDeviceId(const QString &id) {
    if (id == m_deviceId)
        return;
    m_deviceId = id;
    m_settings->setValue("microphone", id);
    emit deviceChanged();
    // Switching microphones mid-meeting restarts capture on the new one.
    if (live()) {
        m_microphone->stop();
        QString error;
        if (!m_microphone->start(m_deviceId.toUtf8(), &error)) {
            pause();
            setProblem(error, "device");
        }
    }
}

QVariantList Backend::providers() const {
    QVariantList list;
    for (Provider *p : m_providers)
        list << QVariantMap{{"id", p->id()}, {"name", p->name()}, {"summary", p->summary()},
                            {"local", p->local()}, {"needsKey", p->needsKey()}, {"keyUrl", keyUrl(p->id())}};
    return list;
}

QVariantMap Backend::keyStates() const {
    QVariantMap states;
    for (const Provider *p : m_providers)
        states.insert(p->id(), keyStateFor(p->id()));
    return states;
}

QString Backend::keyUrl(const QString &id) {
    if (id == "gemini")
        return "https://aistudio.google.com/apikey";
    if (id == "groq")
        return "https://console.groq.com/keys";
    if (id == "openai")
        return "https://platform.openai.com/api-keys";
    return {};
}

QString Backend::providerId() const { return provider()->id(); }
QString Backend::providerName() const { return provider()->name(); }

void Backend::setProviderId(const QString &id) {
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers.at(i)->id() != id || i == m_providerIndex)
            continue;
        const bool wasLive = live();
        if (wasLive)
            pause();
        m_providerIndex = i;
        m_settings->setValue("provider", id);
        m_model = m_settings->value("model/" + id, provider()->defaultModel()).toString();
        if (provider()->model(m_model).id.isEmpty())
            m_model = provider()->defaultModel();
        setProblem({}, {});
        loadKey(id);
        emit providerChanged();
        emit keyChanged();
        return;
    }
}

QVariantList Backend::models() const {
    QVariantList list;
    for (const auto &m : provider()->models())
        list << QVariantMap{{"id", m.id}, {"name", m.name}, {"note", m.note}, {"problem", m.problem}};
    return list;
}

QString Backend::modelName() const {
    return provider()->model(m_model).name;
}

void Backend::setModelId(const QString &id) {
    if (id == m_model || provider()->model(id).id.isEmpty())
        return;
    if (live())
        pause();
    m_model = id;
    m_settings->setValue("model/" + provider()->id(), id);
    setProblem({}, {});
    emit providerChanged();
}

bool Backend::needsKey() const { return provider()->needsKey(); }

QString Backend::keyStateFor(const QString &id) const {
    const Provider *p = nullptr;
    for (const Provider *candidate : m_providers)
        if (candidate->id() == id)
            p = candidate;
    if (!p || !p->needsKey())
        return "none";
    if (m_keySource.value(id) == "checking")
        return "checking";
    return m_keys.value(id).isEmpty() ? "missing" : m_keySource.value(id, "session");
}

void Backend::loadKey(const QString &id) {
    Provider *p = nullptr;
    for (Provider *candidate : m_providers)
        if (candidate->id() == id)
            p = candidate;
    if (!p || !p->needsKey() || !m_keys.value(id).isEmpty() || !m_secrets->available())
        return;
    m_keySource.insert(id, "checking");
    emit keyChanged();
    m_secrets->lookup(id, [this, id](bool ok, const QString &key) {
        if (m_keySource.value(id) != "checking")
            return; // a key typed meanwhile wins
        if (ok) {
            m_keys.insert(id, key);
            m_keySource.insert(id, "saved");
        } else {
            m_keySource.remove(id);
        }
        emit keyChanged();
    });
}

void Backend::setKeyFor(const QString &id, const QString &key, bool remember) {
    const QString trimmed = key.trimmed();
    if (trimmed.isEmpty() || keyStateFor(id) == "none")
        return;
    m_keys.insert(id, trimmed);
    m_keySource.insert(id, "session");
    if (id == providerId() && m_problemAction == "key")
        setProblem({}, {});
    emit keyChanged();
    if (!remember)
        return;
    m_secrets->store(id, trimmed, [this, id](bool ok) {
        if (ok) {
            m_keySource.insert(id, "saved");
            emit keyChanged();
        } else {
            setProblem(tr("The key couldn't be saved to the keyring, so Relay will forget it when it closes."), "key");
        }
    });
}

void Backend::forgetKeyFor(const QString &id) {
    if (keyStateFor(id) == "none")
        return;
    if (id == providerId() && live())
        pause();
    m_keys.remove(id);
    m_keySource.remove(id);
    emit keyChanged();
    m_secrets->remove(id, [](bool) {});
}

void Backend::setChunkSeconds(int seconds) {
    seconds = qBound(4, seconds, 15);
    if (seconds == m_chunkSeconds)
        return;
    m_chunkSeconds = seconds;
    m_settings->setValue("chunkSeconds", seconds);
    applySegmenter();
    emit settingsChanged();
}

void Backend::setFontSize(int size) {
    size = qBound(20, size, 96);
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    m_settings->setValue("fontSize", size);
    emit settingsChanged();
}

void Backend::applySegmenter() {
    Segmenter::Config config = m_segmenter.config();
    config.maxMs = m_chunkSeconds * 1000;
    config.minMs = qMin(2500, config.maxMs / 2);
    m_segmenter.setConfig(config);
}

void Backend::setState(const QString &state) {
    if (state == m_state)
        return;
    m_state = state;
    writeStateFile();
    emit stateChanged();
}

void Backend::setProblem(const QString &text, const QString &action) {
    if (text == m_problem && action == m_problemAction)
        return;
    m_problem = text;
    m_problemAction = action;
    emit problemChanged();
}

// The bar button shows whether Relay is listening by reading this file.
void Backend::writeStateFile() {
    if (m_stateFile.isEmpty())
        return;
    const QString directory = QFileInfo(m_stateFile).absolutePath();
    QDir().mkpath(directory);
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile file(m_stateFile);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(m_state.toUtf8() + "\n");
        file.commit();
    }
}

bool Backend::start() {
    if (live())
        return true;
    Provider *p = provider();
    const ModelInfo model = p->model(m_model);
    if (model.id.isEmpty()) {
        setProblem(tr("Pick a model in Settings."), "model");
        return false;
    }
    if (!model.problem.isEmpty()) {
        setProblem(tr("%1 can't use %2: %3").arg(p->name(), model.name, model.problem), "model");
        return false;
    }
    if (p->needsKey() && m_keys.value(p->id()).isEmpty()) {
        setProblem(keyState() == "checking" ? tr("Waiting for the keyring to unlock.")
                                            : tr("Add a %1 API key in Settings to start.").arg(p->name()),
                   "key");
        return false;
    }

    m_segmenter.reset();
    QString error;
    if (!m_microphone->start(m_deviceId.toUtf8(), &error)) {
        setProblem(error, "device");
        return false;
    }
    if (StreamingProvider *s = stream())
        s->open(m_model, m_keys.value(p->id()));
    else
        m_pipeline.setProvider(p, m_model, m_keys.value(p->id()));
    setProblem({}, {});
    m_failuresInRow = 0;
    setState("live");
    return true;
}

void Backend::pause() {
    if (!live())
        return;
    m_microphone->stop();
    // What was said just before Pause is still worth a caption.
    if (StreamingProvider *s = stream()) {
        m_segmenter.reset();
        s->finish();
    } else if (auto chunk = m_segmenter.flush()) {
        m_audioSeconds += chunk->seconds();
        m_pipeline.submit(*chunk);
    }
    m_level = 0;
    emit levelChanged();
    setState("paused");
    emit activityChanged();
}

void Backend::toggle() {
    if (live())
        pause();
    else
        start();
}

void Backend::shutdown() {
    m_microphone->stop();
    m_segmenter.reset();
    m_pipeline.cancel();
    for (Provider *p : m_providers)
        if (auto *s = qobject_cast<StreamingProvider *>(p))
            s->close();
    setState("paused");
}

void Backend::hear(const QVector<qint16> &samples) {
    if (!live())
        return;
    const bool wasHearing = m_segmenter.inSpeech();
    const auto chunks = m_segmenter.push(samples);
    if (StreamingProvider *s = stream()) {
        // Everything heard goes out as it arrives; the phrases are used only
        // to light the speech indicator.
        s->send(samples);
        m_audioSeconds += samples.size() / 16000.0;
        if (!m_statsTimer.isValid() || m_statsTimer.elapsed() > 1000) {
            m_statsTimer.restart();
            emit statsChanged();
        }
    } else {
        for (const auto &chunk : chunks) {
            m_audioSeconds += chunk.seconds();
            m_pipeline.submit(chunk);
        }
        if (!chunks.isEmpty())
            emit statsChanged();
    }

    // A level meter at 15 updates a second shows the microphone is working.
    const double level = Audio::Converter::level(samples);
    m_level = qMax(level, m_level * 0.7);
    if (!m_levelTimer.isValid() || m_levelTimer.elapsed() > 66 || wasHearing != m_segmenter.inSpeech()) {
        m_levelTimer.restart();
        emit levelChanged();
    }
}

void Backend::received(const Translation &t) {
    m_cost += t.costUsd;
    m_lastLatencyMs = t.latencyMs;
    emit statsChanged();
    if (t.ok) {
        m_failuresInRow = 0;
        if (!t.text.isEmpty())
            m_captions.append(Captions::Caption, t.text, t.language, t.latencyMs);
        if (m_problemAction == "network")
            setProblem({}, {});
        return;
    }
    m_captions.append(Captions::Error, t.error);
    if (t.fatal) {
        // Every further request would fail the same way.
        pause();
        m_pipeline.cancel();
        setProblem(t.error, t.keyRejected ? "key" : "model");
    } else if (++m_failuresInRow >= 3) {
        setProblem(t.error, "network");
    }
}

void Backend::streamed(const QString &text, const QString &language, bool done) {
    m_captions.setLive(text, language);
    if (done)
        m_captions.commitLive();
    if (m_problemAction == "network")
        setProblem({}, {});
}

void Backend::streamFailed(const QString &message, const QString &action) {
    m_captions.append(Captions::Error, message);
    pause();
    setProblem(message, action);
}

void Backend::clearCaptions() {
    m_captions.clear();
}

void Backend::copyCaptions() {
    if (auto *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(m_captions.plainText());
        emit copied();
    }
}
