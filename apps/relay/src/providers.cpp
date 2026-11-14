#include "providers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>

#include "audio.h"

// ---------------------------------------------------------------- Provider

ModelInfo Provider::model(const QString &id) const {
    for (const auto &m : models())
        if (m.id == id)
            return m;
    return {};
}

QString Provider::defaultModel() const {
    const auto all = models();
    for (const auto &m : all)
        if (m.problem.isEmpty())
            return m.id;
    return all.isEmpty() ? QString() : all.first().id;
}

QString Provider::redact(QString text, const QString &key) {
    if (key.size() >= 4)
        text.replace(key, "[key]");
    // OpenAI echoes a masked key ("sk-abc***wxyz"); Groq keys start gsk_,
    // Google keys AIza.
    static const QRegularExpression shaped(R"((?:sk|gsk|sk-proj)[-_][A-Za-z0-9_\-\*]{4,}|AIza[A-Za-z0-9_\-]{8,})");
    text.replace(shaped, "[key]");
    // A URL carrying a key in its query.
    static const QRegularExpression query(R"(([?&]key=)[^&\s]+)");
    text.replace(query, "\\1[key]");
    return text;
}

QString Provider::dropHallucination(const QString &text) {
    // Whisper was trained on subtitled video, and given noise it sometimes
    // writes the credits. These never come from a meeting.
    static const QRegularExpression invented(
        R"(^\W*(thanks? (you )?for watching|please subscribe|like and subscribe|subtitles by|)"
        R"(subtitled by|amara\.org|transcribed by|translated by|captions by)\b.*$)",
        QRegularExpression::CaseInsensitiveOption);
    const QString trimmed = text.trimmed();
    if (invented.match(trimmed).hasMatch())
        return {};
    // Punctuation or a lone symbol is not a caption either.
    static const QRegularExpression wordless(R"(^[\W_]*$)");
    return wordless.match(trimmed).hasMatch() ? QString() : trimmed;
}

// ---------------------------------------------------------------- HTTP

HttpProvider::HttpProvider(const QUrl &baseUrl, QObject *parent) : Provider(parent), m_baseUrl(baseUrl) {}

void HttpProvider::cancelAll() {
    ++m_generation;
    const auto replies = m_replies;
    m_replies.clear();
    for (const auto &reply : replies)
        if (reply)
            reply->abort();
}

void HttpProvider::track(QNetworkReply *reply, Done done) {
    m_replies.append(reply);
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, generation] {
        m_replies.removeAll(reply);
        reply->deleteLater();
        if (generation != m_generation)
            return; // cancelled: Pause throws away what was in flight
        // Bounded: a provider has no business sending megabytes for a caption.
        const QByteArray body = reply->read(1 << 20);
        done(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->error(), body);
    });
}

void HttpProvider::postAudio(const QString &path, const QString &key,
                             const QList<QPair<QByteArray, QByteArray>> &fields, const QByteArray &wav, Done done) {
    QNetworkRequest request(QUrl(m_baseUrl.toString() + path));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setTransferTimeout(m_timeoutMs);

    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto &[name, value] : fields) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"" + name + "\"");
        part.setBody(value);
        multipart->append(part);
    }
    QHttpPart file;
    file.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"file\"; filename=\"speech.wav\"");
    file.setHeader(QNetworkRequest::ContentTypeHeader, "audio/wav");
    file.setBody(wav);
    multipart->append(file);

    QNetworkReply *reply = m_network.post(request, multipart);
    multipart->setParent(reply);
    track(reply, std::move(done));
}

void HttpProvider::postJson(const QString &path, const QString &key, const QJsonObject &body, Done done) {
    QNetworkRequest request(QUrl(m_baseUrl.toString() + path));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(m_timeoutMs);
    track(m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(done));
}

void HttpProvider::finish(Translation result, const QElapsedTimer &timer) {
    result.latencyMs = int(timer.elapsed());
    result.text = dropHallucination(result.text);
    emit finished(result);
}

Translation HttpProvider::parseWhisper(const QByteArray &body) {
    Translation result;
    QJsonParseError error{};
    const QJsonObject object = QJsonDocument::fromJson(body, &error).object();
    if (error.error != QJsonParseError::NoError || !object.contains("text")) {
        result.error = tr("The provider sent a reply Relay doesn't understand.");
        return result;
    }
    result.ok = true;
    // OpenAI's translation reply names the output language ("english"), not
    // the speaker's, so only a non-English name says anything. gpt-transcribe
    // lists what it detected as codes instead.
    QString language = object.value("language").toString();
    const QJsonArray detected = object.value("languages").toArray();
    if (!detected.isEmpty())
        language = detected.first().toObject().value("code").toString();
    language = languageName(language);
    if (!language.isEmpty() && language != "English")
        result.language = language;

    const QJsonArray segments = object.value("segments").toArray();
    if (segments.isEmpty()) {
        result.text = object.value("text").toString().trimmed();
        return result;
    }
    // Whisper's own rule for silence: likely no speech and low confidence.
    // A very compressible segment is the model repeating itself.
    QStringList kept;
    for (const auto &value : segments) {
        const QJsonObject s = value.toObject();
        const bool silent = s.value("no_speech_prob").toDouble() > 0.6 && s.value("avg_logprob").toDouble() < -1.0;
        const bool looping = s.value("compression_ratio").toDouble() > 2.4;
        const QString text = s.value("text").toString().trimmed();
        if (!silent && !looping && !text.isEmpty())
            kept << text;
    }
    result.text = kept.join(' ');
    return result;
}

QString HttpProvider::languageName(const QString &codeOrName) {
    const QString value = codeOrName.trimmed().left(40);
    static const QRegularExpression code("^[a-zA-Z]{2,3}([-_][a-zA-Z]{2,4})?$");
    if (value.size() <= 3 && code.match(value).hasMatch()) {
        const QLocale locale(value);
        if (locale.language() != QLocale::C)
            return QLocale::languageToString(locale.language());
    }
    // Whisper names languages in lower case ("sinhala").
    return value.isEmpty() ? value : value.left(1).toUpper() + value.mid(1);
}

bool HttpProvider::isFatal(int status) {
    return status == 401 || status == 403 || status == 404;
}

QString HttpProvider::describeError(const QString &provider, int status, QNetworkReply::NetworkError error,
                                    const QByteArray &body, const QString &key) {
    QString detail = QJsonDocument::fromJson(body).object().value("error").toObject().value("message").toString();
    detail = redact(detail.left(300), key).trimmed();

    QString message;
    if (status == 401)
        message = tr("%1 rejected the API key. Check it in Settings.").arg(provider);
    else if (status == 403)
        message = tr("%1 refused the request; the key may not have access to this model.").arg(provider);
    else if (status == 404)
        message = tr("%1 doesn't offer this model or endpoint any more.").arg(provider);
    else if (status == 413)
        message = tr("%1 says the audio is too large.").arg(provider);
    else if (status == 429)
        message = tr("%1 is rate limiting or out of credit; some captions will be missing.").arg(provider);
    else if (status >= 500)
        message = tr("%1 had a server error (%2).").arg(provider).arg(status);
    else if (status >= 400)
        message = tr("%1 couldn't use the request (%2).").arg(provider).arg(status);
    else if (error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError)
        message = tr("%1 took too long to answer.").arg(provider);
    else if (error == QNetworkReply::HostNotFoundError || error == QNetworkReply::UnknownNetworkError
             || error == QNetworkReply::TemporaryNetworkFailureError || error == QNetworkReply::NetworkSessionFailedError)
        message = tr("Can't reach %1. Check the network.").arg(provider);
    else if (error != QNetworkReply::NoError)
        message = tr("Can't reach %1.").arg(provider);
    else
        message = tr("%1 sent a reply Relay doesn't understand.").arg(provider);
    return detail.isEmpty() ? message : message + " (" + detail + ")";
}

// ---------------------------------------------------------------- Groq

QString GroqProvider::summary() const {
    return tr("Cloud. Fast and inexpensive; audio goes to Groq.");
}

QList<ModelInfo> GroqProvider::models() const {
    return {
        {"whisper-large-v3", "Whisper Large v3",
         tr("Translates straight to English. About US$0.11 per audio hour; each request bills at least 10 seconds."), {}},
        {"whisper-large-v3-turbo", "Whisper Large v3 Turbo", {},
         tr("Transcribes only. It isn't trained to translate, so it would return the original language.")},
    };
}

double GroqProvider::cost(double seconds, const QString &model) {
    const double perHour = model == "whisper-large-v3-turbo" ? 0.04 : 0.111;
    return qMax(seconds, 10.0) * perHour / 3600.0;
}

void GroqProvider::translate(const TranslationRequest &request, const QString &model, const QString &key) {
    QElapsedTimer timer;
    timer.start();
    const double seconds = request.samples.size() / double(Audio::SampleRate);
    QList<QPair<QByteArray, QByteArray>> fields{
        {"model", model.toUtf8()}, {"response_format", "verbose_json"}, {"temperature", "0"}};
    // Whisper continues the style of its prompt; the last caption keeps names
    // and terms consistent from one chunk to the next. Groq asks for English.
    if (!request.context.isEmpty())
        fields.append({"prompt", request.context.right(400).toUtf8()});

    const quint64 sequence = request.sequence;
    postAudio("/audio/translations", key, fields, Audio::encodeWav(request.samples),
              [this, sequence, seconds, model, key, timer](int status, QNetworkReply::NetworkError error, const QByteArray &body) {
        Translation result;
        if (status == 200)
            result = parseWhisper(body);
        if (status != 200 || !result.ok) {
            result.ok = false;
            result.fatal = isFatal(status);
            result.keyRejected = status == 401;
            result.error = status == 200 ? result.error : describeError(name(), status, error, body, key);
        }
        result.sequence = sequence;
        result.audioSeconds = seconds;
        // A request that reached Groq is billed whatever it returned.
        result.costUsd = status > 0 && status < 500 && status != 401 && status != 429 ? cost(seconds, model) : 0;
        finish(result, timer);
    });
}

// ---------------------------------------------------------------- OpenAI

namespace {
const char *TwoStep = "gpt-transcribe+gpt-6-luna";
}

QString OpenAIProvider::summary() const {
    return tr("Cloud. Audio goes to OpenAI.");
}

QList<ModelInfo> OpenAIProvider::models() const {
    return {
        {TwoStep, "GPT Transcribe + GPT-6 Luna",
         tr("Transcribes, then translates the text, and names the language. About US$0.27 per audio hour; "
            "slower, with two requests per caption."), {}},
        {"whisper-1", "Whisper (whisper-1)",
         tr("Translates straight to English. About US$0.36 per audio hour. OpenAI removes it on 26 February 2027."), {}},
    };
}

double OpenAIProvider::cost(double seconds, const QString &model) {
    // The text step costs a fraction of a cent per hour of captions.
    const double perMinute = model == TwoStep ? 0.0045 : 0.006;
    return seconds / 60.0 * perMinute;
}

void OpenAIProvider::translate(const TranslationRequest &request, const QString &model, const QString &key) {
    QElapsedTimer timer;
    timer.start();
    const double seconds = request.samples.size() / double(Audio::SampleRate);
    const quint64 sequence = request.sequence, generation = m_generation;

    if (model == "whisper-1") {
        QList<QPair<QByteArray, QByteArray>> fields{
            {"model", "whisper-1"}, {"response_format", "verbose_json"}, {"temperature", "0"}};
        if (!request.context.isEmpty())
            fields.append({"prompt", request.context.right(400).toUtf8()});
        postAudio("/audio/translations", key, fields, Audio::encodeWav(request.samples),
                  [this, sequence, seconds, key, timer](int status, QNetworkReply::NetworkError error, const QByteArray &body) {
            Translation result = status == 200 ? parseWhisper(body) : Translation{};
            if (status != 200 || !result.ok) {
                result.ok = false;
                result.fatal = isFatal(status);
            result.keyRejected = status == 401;
                result.error = status == 200 ? result.error : describeError(name(), status, error, body, key);
            }
            result.sequence = sequence;
            result.audioSeconds = seconds;
            result.costUsd = status == 200 ? cost(seconds, "whisper-1") : 0;
            finish(result, timer);
        });
        return;
    }

    // Two steps: the speech as spoken, then that text into English.
    postAudio("/audio/transcriptions", key, {{"model", "gpt-transcribe"}, {"response_format", "json"}},
              Audio::encodeWav(request.samples),
              [this, request, seconds, key, timer, generation](int status, QNetworkReply::NetworkError error, const QByteArray &body) {
        Translation heard = status == 200 ? parseWhisper(body) : Translation{};
        if (status != 200 || !heard.ok || heard.text.isEmpty()) {
            heard.ok = status == 200 && heard.ok; // an empty transcript is a quiet chunk, not an error
            heard.fatal = isFatal(status);
            heard.keyRejected = status == 401;
            if (status != 200)
                heard.error = describeError(name(), status, error, body, key);
            heard.sequence = request.sequence;
            heard.audioSeconds = seconds;
            heard.costUsd = status == 200 ? cost(seconds, TwoStep) : 0;
            finish(heard, timer);
            return;
        }
        if (generation == m_generation)
            translateText(heard.text, heard.language, request, key, timer);
    });
}

void OpenAIProvider::translateText(const QString &original, const QString &heardLanguage,
                                   const TranslationRequest &request, const QString &key, const QElapsedTimer &timer) {
    const double seconds = request.samples.size() / double(Audio::SampleRate);
    QString instructions =
        "You caption a live meeting for an English speaker. The user message is a transcript of a few seconds of "
        "speech, often Sinhala or Tamil, possibly mixed with English. Translate it into natural English, keeping "
        "names, numbers and technical terms. If it is already English, return it unchanged. Name the language "
        "spoken in English (\"Sinhala\", \"Tamil\", \"English\"; for a mix, the main one). Never add commentary.";
    if (!request.context.isEmpty())
        instructions += "\nThe previous caption, for context only: " + request.context.right(400);

    const QJsonObject schema{
        {"type", "object"},
        {"properties", QJsonObject{{"language", QJsonObject{{"type", "string"}}},
                                   {"english", QJsonObject{{"type", "string"}}}}},
        {"required", QJsonArray{"language", "english"}},
        {"additionalProperties", false}};
    const QJsonObject body{
        // Luna is OpenAI's recommended model for quick, high-volume tasks;
        // with no reasoning it answers fastest, which a caption needs.
        {"model", "gpt-6-luna"},
        {"reasoning_effort", "none"},
        {"messages", QJsonArray{QJsonObject{{"role", "developer"}, {"content", instructions}},
                                QJsonObject{{"role", "user"}, {"content", original}}}},
        {"response_format", QJsonObject{{"type", "json_schema"},
                                        {"json_schema", QJsonObject{{"name", "caption"}, {"strict", true},
                                                                    {"schema", schema}}}}}};

    const quint64 sequence = request.sequence;
    postJson("/chat/completions", key, body,
             [this, sequence, seconds, key, timer, heardLanguage](int status, QNetworkReply::NetworkError error, const QByteArray &reply) {
        Translation result = status == 200 ? parseTranslationReply(reply) : Translation{};
        // The speech model heard the language; the text model only guesses.
        if (!heardLanguage.isEmpty())
            result.language = heardLanguage;
        if (result.language == "English")
            result.language.clear();
        if (status != 200 || !result.ok) {
            result.ok = false;
            result.fatal = isFatal(status);
            result.keyRejected = status == 401;
            result.error = status == 200 ? result.error : describeError(name(), status, error, reply, key);
        }
        result.sequence = sequence;
        result.audioSeconds = seconds;
        result.costUsd = cost(seconds, TwoStep);
        finish(result, timer);
    });
}

Translation OpenAIProvider::parseTranslationReply(const QByteArray &body) {
    Translation result;
    const QJsonObject message = QJsonDocument::fromJson(body).object().value("choices").toArray()
                                    .first().toObject().value("message").toObject();
    const QJsonObject content = QJsonDocument::fromJson(message.value("content").toString().toUtf8()).object();
    if (!content.contains("english")) {
        result.error = message.contains("refusal") && !message.value("refusal").toString().isEmpty()
                           ? tr("OpenAI declined to translate this part.")
                           : tr("The provider sent a reply Relay doesn't understand.");
        return result;
    }
    result.ok = true;
    result.text = content.value("english").toString().trimmed();
    result.language = languageName(content.value("language").toString());
    return result;
}

// ---------------------------------------------------------------- Voxtype

VoxtypeProvider::VoxtypeProvider(const QString &program, const QString &modelDirectory, QObject *parent)
    : Provider(parent),
      m_program(!program.isEmpty() ? program : QStandardPaths::findExecutable("voxtype")),
      m_modelDirectory(!modelDirectory.isEmpty() ? modelDirectory
                                                 : QDir::homePath() + "/.local/share/voxtype/models") {
    // Chunks go to a private directory in the runtime dir, which lives in
    // memory and is the user's alone, and are deleted as soon as Voxtype
    // has read them.
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    m_workDirectory = runtime + "/relay";
}

VoxtypeProvider::~VoxtypeProvider() {
    cancelAll();
}

QString VoxtypeProvider::summary() const {
    return tr("On this laptop. Nothing leaves the machine, but captions lag far behind on a laptop CPU.");
}

QList<ModelInfo> VoxtypeProvider::models() const {
    // Only multilingual Whisper translates: the .en models know one
    // language, and large-v3-turbo was never trained to translate.
    struct Entry { const char *id, *name; QString note; };
    const QList<Entry> entries{
        {"tiny", "Whisper tiny", tr("Fastest; too inaccurate for Sinhala and Tamil.")},
        {"base", "Whisper base", tr("Fast; rough for Sinhala and Tamil.")},
        {"small", "Whisper small", tr("Better, but slower than speech on a laptop CPU.")},
        {"medium", "Whisper medium", tr("Much better, and far slower than speech without a GPU.")},
        {"large-v3", "Whisper large-v3", tr("Best local quality; needs a fast GPU to keep up.")},
    };
    QList<ModelInfo> list;
    for (const auto &e : entries) {
        ModelInfo info{e.id, e.name, e.note, {}};
        if (m_program.isEmpty())
            info.problem = tr("Voxtype isn't installed.");
        else if (!QFileInfo::exists(m_modelDirectory + "/ggml-" + e.id + ".bin"))
            info.problem = tr("Not downloaded. Run “voxtype setup model” in a terminal and pick %1.").arg(e.id);
        list << info;
    }
    return list;
}

QString VoxtypeProvider::parseOutput(const QByteArray &stdoutBytes) {
    QStringList lines;
    static const QRegularExpression progress(R"(^(Loading audio file:|Audio format:|Processing \d+ samples))");
    for (const QString &line : QString::fromUtf8(stdoutBytes).split('\n')) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !progress.match(trimmed).hasMatch())
            lines << trimmed;
    }
    return lines.join(' ');
}

void VoxtypeProvider::cancelAll() {
    const auto processes = m_processes;
    m_processes.clear();
    for (const auto &process : processes) {
        if (!process)
            continue;
        process->disconnect(this);
        process->kill();
        process->waitForFinished(1000);
        process->deleteLater();
    }
}

void VoxtypeProvider::translate(const TranslationRequest &request, const QString &model, const QString &) {
    QElapsedTimer timer;
    timer.start();
    Translation result;
    result.sequence = request.sequence;
    result.audioSeconds = request.samples.size() / double(Audio::SampleRate);

    const QString problem = this->model(model).problem;
    QDir().mkpath(m_workDirectory);
    QFile::setPermissions(m_workDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    auto *file = new QTemporaryFile(m_workDirectory + "/chunk-XXXXXX.wav", this);
    if (!problem.isEmpty() || !file->open() || file->write(Audio::encodeWav(request.samples)) < 0 || !file->flush()) {
        result.fatal = !problem.isEmpty();
        result.error = !problem.isEmpty() ? problem : tr("Relay couldn't write audio for Voxtype.");
        delete file;
        // Finish on the next turn of the event loop, like a real request.
        QTimer::singleShot(0, this, [this, result, timer] { Translation r = result; r.latencyMs = int(timer.elapsed()); emit finished(r); });
        return;
    }
    file->close();

    auto *process = new QProcess(this);
    m_processes.append(process);
    file->setParent(process); // deleted, with the audio, along with the process
    const int threads = qMax(1, QThread::idealThreadCount());
    process->setProgram(m_program);
    process->setArguments({"--quiet", "--model", model, "--translate", "--language", "auto",
                           "--threads", QString::number(threads), "transcribe", file->fileName()});

    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    auto done = [this, process, result, timer] {
        if (!m_processes.removeAll(process))
            return;
        Translation r = result;
        const QByteArray out = process->readAllStandardOutput().left(1 << 20);
        if (process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0) {
            r.ok = true;
            r.text = dropHallucination(parseOutput(out));
        } else if (process->error() == QProcess::FailedToStart) {
            r.fatal = true;
            r.error = tr("Voxtype couldn't start.");
        } else if (process->exitStatus() == QProcess::CrashExit) {
            r.error = tr("Voxtype took too long or stopped unexpectedly.");
        } else {
            const QString err = QString::fromUtf8(process->readAllStandardError().right(2000));
            const QString last = err.split('\n', Qt::SkipEmptyParts).filter(QRegularExpression("^Error")).value(0);
            r.error = tr("Voxtype failed: %1").arg(last.isEmpty() ? tr("exit code %1").arg(process->exitCode()) : last);
        }
        r.latencyMs = int(timer.elapsed());
        process->deleteLater();
        emit finished(r);
    };
    connect(process, &QProcess::finished, this, done);
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done();
        else if (process->state() == QProcess::NotRunning)
            done();
    });
    process->start();
    timeout->start(m_timeoutMs);
}
