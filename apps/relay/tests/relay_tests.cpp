#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <cmath>

#include "audio.h"
#include "backend.h"
#include "captions.h"
#include "live.h"
#include "pipeline.h"
#include "providers.h"
#include "secrets.h"
#include "segmenter.h"
#include "theme.h"
#include "translator.h"

namespace {

// Speech-like audio: a voiced tone loud enough to stand out from the room.
QVector<qint16> tone(int ms, double amplitude = 0.3, int hz = 220) {
    QVector<qint16> out(16 * ms);
    for (int i = 0; i < out.size(); ++i)
        out[i] = qint16(amplitude * 32767 * std::sin(2 * M_PI * hz * i / 16000.0));
    return out;
}

// Speech-like audio: voiced stretches with the short dips between
// syllables that real speech always has (about four a second).
QVector<qint16> speech(int ms, double amplitude = 0.3);

// A quiet room: low, deterministic noise.
QVector<qint16> room(int ms, double amplitude = 0.002) {
    QVector<qint16> out(16 * ms);
    quint32 seed = 12345;
    for (auto &s : out) {
        seed = seed * 1103515245 + 12345;
        s = qint16(amplitude * 32767 * (((seed >> 16) & 0x7fff) / 16383.5 - 1.0));
    }
    return out;
}

QVector<qint16> speech(int ms, double amplitude) {
    QVector<qint16> out;
    for (int t = 0, i = 0; t < ms; t += 250, ++i) {
        const int voiced = qMin(190, ms - t), dip = qMin(60, ms - t - voiced);
        out += tone(voiced, amplitude * (0.8 + 0.1 * (i % 3)), 160 + 25 * (i % 5));
        if (dip > 0)
            out += room(dip);
    }
    return out;
}

bool writeFile(const QString &path, const QByteArray &data, bool executable = false) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        return false;
    file.close();
    if (executable)
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A provider the test answers by hand.
class FakeProvider : public Provider {
public:
    explicit FakeProvider(int concurrent = 2, bool key = false) : m_concurrent(concurrent), m_key(key) {}
    QString id() const override { return objectName().isEmpty() ? QString("fake") : objectName(); }
    QString name() const override { return "Fake"; }
    QString summary() const override { return {}; }
    bool needsKey() const override { return m_key; }
    QList<ModelInfo> models() const override { return {{"m", "Model", {}, {}}}; }
    int maxConcurrent() const override { return m_concurrent; }
    void translate(const TranslationRequest &request, const QString &, const QString &) override {
        requests << request;
    }
    void cancelAll() override { requests.clear(); ++cancels; }
    void reply(quint64 sequence, const QString &text, bool ok = true, bool fatal = false, bool keyRejected = false) {
        Translation t;
        t.sequence = sequence;
        t.ok = ok;
        t.fatal = fatal;
        t.keyRejected = keyRejected;
        t.text = ok ? text : QString();
        t.error = ok ? QString() : text;
        emit finished(t);
    }
    QList<TranslationRequest> requests;
    int cancels = 0;

private:
    int m_concurrent;
    bool m_key;
};

class FakeMicrophone : public Microphone {
public:
    bool start(const QByteArray &deviceId, QString *) override { m_active = true; device = deviceId; ++starts; return true; }
    void stop() override { m_active = false; }
    bool active() const override { return m_active; }
    QByteArray device;
    int starts = 0;

private:
    bool m_active = false;
};

// A tiny HTTP/1.1 server: records each request and answers by path.
class FakeHttp : public QObject {
public:
    struct Request { QByteArray method, path, headers, body; };
    QHash<QByteArray, QPair<int, QByteArray>> responses;
    QList<Request> requests;

    FakeHttp() {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end);
                    const auto length = QRegularExpression("(?i)content-length:\\s*(\\d+)")
                                            .match(QString::fromLatin1(head)).captured(1).toInt();
                    if (buffer->size() < end + 4 + length)
                        return;
                    Request r;
                    const QList<QByteArray> first = head.left(head.indexOf("\r\n")).split(' ');
                    r.method = first.value(0);
                    r.path = first.value(1);
                    r.headers = head;
                    r.body = buffer->mid(end + 4, length);
                    requests << r;
                    const auto response = responses.value(r.path, {404, R"({"error":{"message":"no route"}})"});
                    socket->write("HTTP/1.1 " + QByteArray::number(response.first) + " X\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(response.second.size()) +
                                  "\r\nConnection: close\r\n\r\n" + response.second);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/v1").arg(m_server.serverPort())); }

private:
    QTcpServer m_server;
};

QVector<qint16> threeSecondsOfSpeech() { return tone(3000); }

// A stand-in for the Live API: records what each connection sends and
// lets the test answer, in binary frames as Google does.
class FakeLive : public QObject {
public:
    struct Session { QWebSocket *socket; QUrl url; QList<QJsonObject> received; };
    QList<Session> sessions;

    FakeLive() : m_server("fake-live", QWebSocketServer::NonSecureMode) {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QWebSocketServer::newConnection, this, [this] {
            while (QWebSocket *socket = m_server.nextPendingConnection()) {
                const int index = int(sessions.size());
                sessions << Session{socket, socket->requestUrl(), {}};
                connect(socket, &QWebSocket::textMessageReceived, this, [this, index](const QString &message) {
                    sessions[index].received << QJsonDocument::fromJson(message.toUtf8()).object();
                });
            }
        });
    }
    QUrl url() const { return QUrl(QString("ws://127.0.0.1:%1/ws/live").arg(m_server.serverPort())); }
    void reply(const QByteArray &json, int session = -1) {
        sessions[session < 0 ? sessions.size() - 1 : session].socket->sendBinaryMessage(json);
    }
    QList<QJsonObject> audio(int session = -1) const {
        QList<QJsonObject> out;
        for (const auto &m : sessions[session < 0 ? sessions.size() - 1 : session].received)
            if (m.value("realtimeInput").toObject().contains("audio"))
                out << m.value("realtimeInput").toObject().value("audio").toObject();
        return out;
    }

private:
    QWebSocketServer m_server;
};

} // namespace

class RelayTests : public QObject {
    Q_OBJECT

private slots:
    // ------------------------------------------------------------ audio

    void converterPassesThroughSixteenKilohertzMono() {
        QAudioFormat format;
        format.setSampleRate(16000);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        Audio::Converter converter(format);
        const auto input = tone(100);
        const auto output = converter.convert(reinterpret_cast<const char *>(input.constData()), input.size() * 2);
        // Interpolation holds back one sample for the next buffer.
        QVERIFY(qAbs(output.size() - input.size()) <= 2);
        for (int i = 0; i < output.size(); ++i)
            QVERIFY(qAbs(output[i] - input[i]) <= 1);
    }

    void converterDownmixesAndResamplesInPieces() {
        // 48 kHz stereo float, the usual PipeWire format, fed in odd pieces.
        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Float);
        QVector<float> stereo;
        for (int i = 0; i < 48000; ++i) {
            const float v = 0.5f * std::sin(2 * M_PI * 440 * i / 48000.0);
            stereo << v << v;
        }
        const char *bytes = reinterpret_cast<const char *>(stereo.constData());
        const qsizetype total = stereo.size() * sizeof(float);

        Audio::Converter whole(format), pieces(format);
        const auto a = whole.convert(bytes, total);
        QVector<qint16> b;
        for (qsizetype offset = 0, step = 8 * 997; offset < total; offset += step)
            b += pieces.convert(bytes + offset, qMin(step, total - offset));

        QVERIFY2(qAbs(a.size() - 16000) <= 4, qPrintable(QString::number(a.size())));
        QVERIFY(qAbs(a.size() - b.size()) <= 2);
        // Amplitude survives: the level of a 0.5 sine is 0.5/sqrt(2).
        QVERIFY(qAbs(Audio::Converter::level(a) - 0.3535) < 0.02);
    }

    void wavHasACorrectHeader() {
        const auto samples = tone(500);
        const QByteArray wav = Audio::encodeWav(samples);
        QCOMPARE(wav.size(), 44 + samples.size() * 2);
        QCOMPARE(wav.left(4), QByteArray("RIFF"));
        QCOMPARE(wav.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 24), 16000u);
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 22), quint16(1));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 40), quint32(samples.size() * 2));
    }

    // ------------------------------------------------------------ segmenter

    void segmenterSendsNoSilence() {
        Segmenter segmenter;
        QVERIFY(segmenter.push(room(10000)).isEmpty());
        QVERIFY(!segmenter.flush().has_value());
    }

    void segmenterIgnoresAClick() {
        Segmenter segmenter;
        QVector<qint16> audio = room(2000) + tone(100, 0.6) + room(2000);
        QVERIFY(segmenter.push(audio).isEmpty());
        QVERIFY(!segmenter.flush().has_value());
    }

    void segmenterCutsAtPauses() {
        Segmenter segmenter;
        auto chunks = segmenter.push(room(1000) + speech(3000) + room(1000) + speech(2000));
        QCOMPARE(chunks.size(), 1);
        // The first phrase with a little silence either side, not the room.
        QVERIFY2(chunks[0].seconds() > 3.0 && chunks[0].seconds() < 3.8, qPrintable(QString::number(chunks[0].seconds())));
        QVERIFY(chunks[0].speechMs >= 2000); // the dips between syllables are not speech
        const auto last = segmenter.flush();
        QVERIFY(last.has_value());
        QVERIFY(last->seconds() > 1.9 && last->seconds() < 2.6);
    }

    void segmenterCapsLongSpeechWithoutLosingAny() {
        Segmenter::Config config;
        config.maxMs = 8000;
        Segmenter segmenter(config);
        // A long talk with no pause long enough to end a phrase, only the
        // brief dips between syllables that real speech always has.
        QVector<qint16> talk;
        for (int i = 0; i < 20; ++i)
            talk += speech(1000, 0.3 + 0.05 * (i % 3));
        auto chunks = segmenter.push(talk);
        if (auto rest = segmenter.flush())
            chunks << *rest;
        QVERIFY(chunks.size() >= 3);
        double total = 0;
        for (const auto &chunk : chunks) {
            QVERIFY(chunk.seconds() <= 8.0 + 0.001);
            total += chunk.seconds();
        }
        QVERIFY2(qAbs(total - 20.0) < 0.05, qPrintable(QString::number(total)));
    }

    void segmenterAdaptsToANoisyRoom() {
        // A loud fan becomes background; speech above it still counts.
        Segmenter segmenter;
        QVERIFY(segmenter.push(room(4000, 0.03)).isEmpty());
        auto mix = speech(3000, 0.3);
        const auto fan = room(3000, 0.03);
        for (int i = 0; i < mix.size(); ++i)
            mix[i] = qint16(qBound(-32768, mix[i] + fan[i], 32767));
        auto chunks = segmenter.push(mix + room(1000, 0.03));
        QCOMPARE(chunks.size(), 1);
        QVERIFY(chunks[0].seconds() < 4.0);
    }

    // ------------------------------------------------------------ pipeline

    void pipelineDeliversInSpokenOrder() {
        FakeProvider provider(3);
        Pipeline pipeline;
        pipeline.setProvider(&provider, "m", {});
        QStringList delivered;
        connect(&pipeline, &Pipeline::result, this, [&](const Translation &t) { delivered << t.text; });
        for (int i = 0; i < 3; ++i)
            pipeline.submit({threeSecondsOfSpeech(), 3000});
        QCOMPARE(provider.requests.size(), 3);
        provider.reply(3, "three");
        provider.reply(2, "two");
        QVERIFY(delivered.isEmpty()); // still waiting for the first words
        provider.reply(1, "one");
        QCOMPARE(delivered, QStringList({"one", "two", "three"}));
        QVERIFY(pipeline.idle());
    }

    void pipelineFailureDoesNotBlockLaterCaptions() {
        FakeProvider provider(2);
        Pipeline pipeline;
        pipeline.setProvider(&provider, "m", {});
        QList<Translation> delivered;
        connect(&pipeline, &Pipeline::result, this, [&](const Translation &t) { delivered << t; });
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        provider.reply(2, "after");
        provider.reply(1, "timed out", false);
        QCOMPARE(delivered.size(), 2);
        QVERIFY(!delivered[0].ok);
        QCOMPARE(delivered[1].text, QString("after"));
    }

    void pipelineDropsOldestAudioWhenBehind() {
        FakeProvider provider(1);
        Pipeline pipeline;
        pipeline.setMaxWaiting(2);
        pipeline.setProvider(&provider, "m", {});
        QSignalSpy skipped(&pipeline, &Pipeline::skipped);
        QStringList delivered;
        connect(&pipeline, &Pipeline::result, this, [&](const Translation &t) { delivered << t.text; });
        for (int i = 0; i < 5; ++i)
            pipeline.submit({threeSecondsOfSpeech(), 3000});
        // One in flight, two waiting; chunks 2 and 3 were dropped.
        QCOMPARE(skipped.count(), 2);
        QCOMPARE(qRound(skipped.at(0).at(0).toDouble()), 3);
        QCOMPARE(pipeline.waiting(), 2);
        provider.reply(1, "one");
        QCOMPARE(provider.requests.last().sequence, quint64(4));
        provider.reply(4, "four");
        provider.reply(5, "five");
        QCOMPARE(delivered, QStringList({"one", "four", "five"}));
    }

    void pipelineCancelDropsLateReplies() {
        FakeProvider provider(2);
        Pipeline pipeline;
        pipeline.setProvider(&provider, "m", {});
        QSignalSpy results(&pipeline, &Pipeline::result);
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        pipeline.cancel();
        QCOMPARE(provider.cancels, 1);
        provider.reply(1, "stale");
        QCOMPARE(results.count(), 0);
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        provider.reply(2, "fresh");
        QCOMPARE(results.count(), 1);
    }

    void pipelinePassesThePreviousCaptionAsContext() {
        FakeProvider provider(1);
        Pipeline pipeline;
        pipeline.setProvider(&provider, "m", {});
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        pipeline.submit({threeSecondsOfSpeech(), 3000});
        QVERIFY(provider.requests[0].context.isEmpty());
        provider.reply(1, "The budget for Jaffna is approved.");
        QCOMPARE(provider.requests[1].context, QString("The budget for Jaffna is approved."));
    }

    // ------------------------------------------------------------ parsing

    void whisperRepliesDropSilentAndLoopingSegments() {
        const QByteArray body = R"({"text":"x","language":"sinhala","segments":[
            {"text":" The meeting starts now.","no_speech_prob":0.01,"avg_logprob":-0.2,"compression_ratio":1.3},
            {"text":" Thank you.","no_speech_prob":0.9,"avg_logprob":-1.4,"compression_ratio":1.0},
            {"text":" yes yes yes yes yes yes yes yes","no_speech_prob":0.1,"avg_logprob":-0.3,"compression_ratio":3.1}]})";
        const Translation t = HttpProvider::parseWhisper(body);
        QVERIFY(t.ok);
        QCOMPARE(t.text, QString("The meeting starts now."));
        QCOMPARE(t.language, QString("Sinhala"));
    }

    void whisperRepliesNamingEnglishSayNothingAboutTheSpeaker() {
        const Translation t = HttpProvider::parseWhisper(R"({"text":" Hello.","language":"english"})");
        QVERIFY(t.ok);
        QCOMPARE(t.text, QString("Hello."));
        QVERIFY(t.language.isEmpty());
        QVERIFY(!HttpProvider::parseWhisper("<html>gateway</html>").ok);
    }

    void languageNamesFromCodesAndWords() {
        QCOMPARE(HttpProvider::languageName("si"), QString("Sinhala"));
        QCOMPARE(HttpProvider::languageName("ta"), QString("Tamil"));
        QCOMPARE(HttpProvider::languageName("tamil"), QString("Tamil"));
    }

    void inventedCaptionsAreDropped() {
        QVERIFY(Provider::dropHallucination("Thanks for watching!").isEmpty());
        QVERIFY(Provider::dropHallucination(" Subtitles by the Amara.org community").isEmpty());
        QVERIFY(Provider::dropHallucination("...").isEmpty());
        QCOMPARE(Provider::dropHallucination(" Thank you, everyone. "), QString("Thank you, everyone."));
    }

    void errorsNeverShowTheKey() {
        const QString key = "example-key";
        const QByteArray body = R"j({"error":{"message":"Invalid API Key example-key (sk-proj-abc***wxyz)"}})j";
        const QString message = HttpProvider::describeError("Groq", 401, QNetworkReply::AuthenticationRequiredError, body, key);
        QVERIFY(message.contains("rejected the API key"));
        QVERIFY(!message.contains("example-key"));
        QVERIFY(!message.contains("abc***"));
        QVERIFY(HttpProvider::isFatal(401));
        QVERIFY(!HttpProvider::isFatal(429));
        QVERIFY(HttpProvider::describeError("Groq", 0, QNetworkReply::HostNotFoundError, {}, key).contains("Can't reach"));
        QVERIFY(HttpProvider::describeError("Groq", 429, QNetworkReply::NoError, {}, key).contains("rate limiting"));
    }

    void groqMinimumBillIsTenSeconds() {
        QCOMPARE(GroqProvider::cost(3, "whisper-large-v3"), GroqProvider::cost(10, "whisper-large-v3"));
        QVERIFY(qAbs(GroqProvider::cost(3600, "whisper-large-v3") - 0.111) < 1e-9);
    }

    void groqTurboIsNotOfferedForTranslation() {
        GroqProvider groq;
        QCOMPARE(groq.defaultModel(), QString("whisper-large-v3"));
        QVERIFY(!groq.model("whisper-large-v3-turbo").problem.isEmpty());
    }

    // ------------------------------------------------------------ HTTP

    void groqTranslatesOverHttp() {
        FakeHttp server;
        server.responses["/v1/audio/translations"] = {200, R"({"text":" We need the report by Friday.","segments":[
            {"text":" We need the report by Friday.","no_speech_prob":0.02,"avg_logprob":-0.25,"compression_ratio":1.2}]})"};
        GroqProvider groq(server.url());
        QSignalSpy finished(&groq, &Provider::finished);
        groq.translate({7, tone(3000), "Earlier caption."}, "whisper-large-v3", "gsk_test");
        QVERIFY(finished.wait(5000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY2(t.ok, qPrintable(t.error));
        QCOMPARE(t.sequence, quint64(7));
        QCOMPARE(t.text, QString("We need the report by Friday."));
        QVERIFY(qAbs(t.costUsd - GroqProvider::cost(10, "whisper-large-v3")) < 1e-12);

        QCOMPARE(server.requests.size(), 1);
        const auto &r = server.requests[0];
        QCOMPARE(r.method, QByteArray("POST"));
        QVERIFY(r.headers.contains("Authorization: Bearer gsk_test"));
        QVERIFY(r.body.contains("name=\"model\"\r\n\r\nwhisper-large-v3\r\n"));
        QVERIFY(r.body.contains("name=\"response_format\"\r\n\r\nverbose_json"));
        QVERIFY(r.body.contains("name=\"prompt\"\r\n\r\nEarlier caption."));
        QVERIFY(r.body.contains("RIFF"));
    }

    void rejectedKeyIsFatal() {
        FakeHttp server;
        server.responses["/v1/audio/translations"] = {401, R"({"error":{"message":"Invalid API Key"}})"};
        GroqProvider groq(server.url());
        QSignalSpy finished(&groq, &Provider::finished);
        groq.translate({1, tone(2000), {}}, "whisper-large-v3", "gsk_bad");
        QVERIFY(finished.wait(5000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY(!t.ok);
        QVERIFY(t.fatal);
        QVERIFY(t.keyRejected);
        QCOMPARE(t.costUsd, 0.0);
    }

    void unreachableProviderIsReported() {
        GroqProvider groq(QUrl("http://127.0.0.1:9/v1"));
        QSignalSpy finished(&groq, &Provider::finished);
        groq.translate({1, tone(2000), {}}, "whisper-large-v3", "k");
        QVERIFY(finished.wait(10000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY(!t.ok);
        QVERIFY(!t.fatal);
        QVERIFY2(t.error.contains("Can't reach"), qPrintable(t.error));
    }

    void openAiTranscribesThenTranslates() {
        FakeHttp server;
        server.responses["/v1/audio/transcriptions"] = {200, R"({"text":"අපි සිකුරාදා වාර්තාව ඉල්ලනවා","languages":[{"code":"si"}]})"};
        // Built rather than written out: moc misreads escaped quotes in raw strings.
        const QJsonObject content{{"language", "Sinhala"}, {"english", "We are asking for the report on Friday."}};
        const QJsonObject message{{"role", "assistant"}, {"content", QString::fromUtf8(QJsonDocument(content).toJson())}};
        server.responses["/v1/chat/completions"] = {200, QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", message}}}}}).toJson()};
        OpenAIProvider openai(server.url());
        QSignalSpy finished(&openai, &Provider::finished);
        openai.translate({3, tone(3000), {}}, openai.defaultModel(), "sk-test");
        QVERIFY(finished.wait(5000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY2(t.ok, qPrintable(t.error));
        QCOMPARE(t.text, QString("We are asking for the report on Friday."));
        QCOMPARE(t.language, QString("Sinhala"));
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(server.requests[0].body.contains("name=\"model\"\r\n\r\ngpt-transcribe\r\n"));
        const QJsonObject chat = QJsonDocument::fromJson(server.requests[1].body).object();
        QCOMPARE(chat.value("model").toString(), QString("gpt-6-luna"));
        QCOMPARE(chat.value("reasoning_effort").toString(), QString("none"));
        QVERIFY(chat.value("messages").toArray().at(1).toObject().value("content").toString().contains("වාර්තාව"));
    }

    void openAiWhisperUsesTheTranslationsEndpoint() {
        FakeHttp server;
        server.responses["/v1/audio/translations"] = {200, R"({"text":" Good morning."})"};
        OpenAIProvider openai(server.url());
        QSignalSpy finished(&openai, &Provider::finished);
        openai.translate({1, tone(2000), {}}, "whisper-1", "sk-test");
        QVERIFY(finished.wait(5000));
        QCOMPARE(finished.at(0).at(0).value<Translation>().text, QString("Good morning."));
        QVERIFY(server.requests[0].body.contains("whisper-1"));
    }

    // ------------------------------------------------------------ Voxtype

    void voxtypeExplainsMissingModels() {
        QTemporaryDir dir;
        const QString program = dir.filePath("voxtype");
        QVERIFY(writeFile(program, "#!/bin/sh\nexit 0\n", true));
        QVERIFY(writeFile(dir.filePath("ggml-small.bin"), "x"));
        VoxtypeProvider voxtype(program, dir.path());
        QVERIFY(!voxtype.model("base").problem.isEmpty());
        QVERIFY(voxtype.model("small").problem.isEmpty());
        QCOMPARE(voxtype.defaultModel(), QString("small"));
        // No English-only or turbo models: they can't translate.
        for (const auto &m : voxtype.models())
            QVERIFY(!m.id.endsWith(".en") && !m.id.contains("turbo"));

        QSignalSpy finished(&voxtype, &Provider::finished);
        voxtype.translate({1, tone(1000), {}}, "base", {});
        QVERIFY(finished.wait(2000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY(t.fatal);
        QVERIFY(t.error.contains("voxtype setup model"));
    }

    void voxtypeTranslatesAndDeletesTheAudio() {
        QTemporaryDir dir;
        const QString program = dir.filePath("voxtype"), log = dir.filePath("args");
        // Stands in for Voxtype: records its arguments and whether the file
        // exists while it runs, then prints what the real one prints.
        QVERIFY(writeFile(program, "#!/bin/sh\nfor a; do last=\"$a\"; done\n"
                                   "echo \"$@\" > '" + log.toUtf8() + "'\n"
                                   "test -s \"$last\" && echo present >> '" + log.toUtf8() + "'\n"
                                   "echo 'Loading audio file: \"x.wav\"'\necho 'Audio format: 16000 Hz, 1 channel(s), Int'\n"
                                   "echo 'Processing 32000 samples (2.00s)...'\necho\necho ' Please send the minutes.'\n", true));
        QVERIFY(writeFile(dir.filePath("ggml-small.bin"), "x"));
        VoxtypeProvider voxtype(program, dir.path());
        QSignalSpy finished(&voxtype, &Provider::finished);
        voxtype.translate({4, tone(2000), {}}, "small", {});
        QVERIFY(finished.wait(5000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY2(t.ok, qPrintable(t.error));
        QCOMPARE(t.text, QString("Please send the minutes."));
        const QString args = QString::fromUtf8(readFile(log));
        QVERIFY(args.contains("--model small --translate --language auto"));
        QVERIFY(args.contains("present"));
        const QString audioPath = args.split('\n').first().split(' ').last();
        QTRY_VERIFY(!QFile::exists(audioPath));
    }

    void voxtypeFailureIsReported() {
        QTemporaryDir dir;
        const QString program = dir.filePath("voxtype");
        QVERIFY(writeFile(program, "#!/bin/sh\necho 'Error: Model not found' >&2\nexit 1\n", true));
        QVERIFY(writeFile(dir.filePath("ggml-base.bin"), "x"));
        VoxtypeProvider voxtype(program, dir.path());
        QSignalSpy finished(&voxtype, &Provider::finished);
        voxtype.translate({1, tone(1000), {}}, "base", {});
        QVERIFY(finished.wait(5000));
        const auto t = finished.at(0).at(0).value<Translation>();
        QVERIFY(!t.ok);
        QVERIFY2(t.error.contains("Model not found"), qPrintable(t.error));
    }

    // ------------------------------------------------------------ secrets

    void secretsPassKeysOnStdinOnly() {
        QTemporaryDir dir;
        const QString tool = dir.filePath("secret-tool"), store = dir.filePath("store"), args = dir.filePath("args");
        QVERIFY(writeFile(tool, "#!/bin/sh\necho \"$@\" >> '" + args.toUtf8() + "'\n"
                                "case \"$1\" in\n"
                                "store) cat > '" + store.toUtf8() + "';;\n"
                                "lookup) test -f '" + store.toUtf8() + "' && cat '" + store.toUtf8() + "' || exit 1;;\n"
                                "clear) rm -f '" + store.toUtf8() + "';;\nesac\n", true));
        Secrets secrets(tool);
        bool stored = false, found = false, removed = false;
        QString key;
        secrets.store("groq", "gsk_topsecret", [&](bool ok) { stored = ok; });
        QTRY_VERIFY(stored);
        secrets.lookup("groq", [&](bool ok, const QString &k) { found = ok; key = k; });
        QTRY_VERIFY(found);
        QCOMPARE(key, QString("gsk_topsecret"));
        QVERIFY(!readFile(args).contains("topsecret"));
        QVERIFY(readFile(args).contains("application relay provider groq"));
        secrets.remove("groq", [&](bool ok) { removed = ok; });
        QTRY_VERIFY(removed);
        bool done = false;
        secrets.lookup("groq", [&](bool ok, const QString &) { found = ok; done = true; });
        QTRY_VERIFY(done);
        QVERIFY(!found);
    }

    // ------------------------------------------------------------ gemini

    void geminiAsksForEnglishCaptions() {
        const auto setup = QJsonDocument::fromJson(GeminiLiveProvider::setupMessage("gemini-3.5-live-translate-preview", {}))
                               .object().value("setup").toObject();
        QCOMPARE(setup.value("model").toString(), QString("models/gemini-3.5-live-translate-preview"));
        const auto generation = setup.value("generationConfig").toObject();
        QCOMPARE(generation.value("translationConfig").toObject().value("targetLanguageCode").toString(), QString("en"));
        QVERIFY(generation.value("translationConfig").toObject().value("echoTargetLanguage").toBool());
        QVERIFY(setup.contains("outputAudioTranscription")); // not in generationConfig: the server refuses it there
        QVERIFY(setup.contains("contextWindowCompression"));
        QVERIFY(!setup.value("sessionResumption").toObject().contains("handle"));
        const auto resumed = QJsonDocument::fromJson(GeminiLiveProvider::setupMessage("m", "h1")).object();
        QCOMPARE(resumed.value("setup").toObject().value("sessionResumption").toObject().value("handle").toString(), QString("h1"));
    }

    void captionsGrowAndBreakAtSentences() {
        CaptionAssembler a(60);
        QVERIFY(a.add("Good").isEmpty());
        QVERIFY(a.add(" morning,").isEmpty());
        QCOMPARE(a.current(), QString("Good morning,"));
        QVERIFY(a.add("Good morning, everyone").isEmpty()); // a repeat of everything so far
        QCOMPARE(a.current(), QString("Good morning, everyone"));
        const QStringList done = a.add(". The budget for next quarter is ready, and we start on Monday.");
        QCOMPARE(done, QStringList{"Good morning, everyone."});
        QCOMPARE(a.take(), QString("The budget for next quarter is ready, and we start on Monday."));
        QVERIFY(a.isEmpty());
        // A long run with no sentence end at all still breaks.
        CaptionAssembler b(20);
        const QStringList pieces = b.add("one two three four five six seven eight nine ten eleven twelve");
        QCOMPARE(pieces.size(), 1);
        QVERIFY(pieces.first().size() < 70);
    }

    void geminiStreamsAudioAndCaptionsAsTheyCome() {
        FakeLive server;
        GeminiLiveProvider gemini(server.url());
        QSignalSpy captions(&gemini, &StreamingProvider::caption);
        gemini.open("gemini-3.5-live-translate-preview", "AIzaSyTestKey000000000000000000000000");
        gemini.send(tone(250)); // before setup: waits
        QTRY_COMPARE(server.sessions.size(), 1);
        QVERIFY(server.sessions[0].url.query().contains("key=AIzaSyTestKey"));
        QTRY_COMPARE(server.sessions[0].received.size(), 1);
        QVERIFY(server.sessions[0].received[0].contains("setup"));
        QVERIFY(!gemini.ready());

        server.reply(R"({"setupComplete":{}})");
        QTRY_VERIFY(gemini.ready());
        QTRY_COMPARE(server.audio().size(), 2); // 250 ms: two 100 ms frames, 50 ms kept back
        const QJsonObject frame = server.audio().first();
        QCOMPARE(frame.value("mimeType").toString(), QString("audio/pcm;rate=16000"));
        QCOMPARE(QByteArray::fromBase64(frame.value("data").toString().toLatin1()).size(), 3200);

        server.reply(R"({"serverContent":{"inputTranscription":{"text":"x","languageCode":"si"}}})");
        server.reply(R"({"serverContent":{"outputTranscription":{"text":"Good","languageCode":"en"}}})");
        server.reply(R"({"serverContent":{"outputTranscription":{"text":" morning.","languageCode":"en"}}})");
        server.reply(R"({"serverContent":{"turnComplete":true}})");
        QTRY_COMPARE(captions.size(), 3);
        QCOMPARE(captions[0], (QVariantList{"Good", "Sinhala", false}));
        QCOMPARE(captions[1], (QVariantList{"Good morning.", "Sinhala", false}));
        QCOMPARE(captions[2], (QVariantList{"Good morning.", "Sinhala", true}));

        // Pause: the rest of the audio and an end of stream, then the last words.
        gemini.setFinishGraceMs(300);
        gemini.send(tone(30));
        gemini.finish();
        QTRY_VERIFY(server.sessions[0].received.last().value("realtimeInput").toObject().value("audioStreamEnd").toBool());
        server.reply(R"({"serverContent":{"outputTranscription":{"text":"Thank you"}}})");
        QTRY_COMPARE(captions.size(), 5);
        QCOMPARE(captions[4], (QVariantList{"Thank you", "Sinhala", true}));
        QTRY_COMPARE(server.sessions[0].socket->state(), QAbstractSocket::UnconnectedState);
    }

    void geminiResumesWhenTheServerMovesOn() {
        FakeLive server;
        GeminiLiveProvider gemini(server.url());
        gemini.setRetryDelayMs(10);
        gemini.open("m", "AIzaSyTestKey000000000000000000000000");
        QTRY_COMPARE(server.sessions.size(), 1);
        server.reply(R"({"setupComplete":{}})");
        QTRY_VERIFY(gemini.ready());
        server.reply(R"({"sessionResumptionUpdate":{"newHandle":"handle-1","resumable":true}})");
        server.reply(R"({"goAway":{"timeLeft":"5s"}})");
        QTRY_COMPARE(server.sessions.size(), 2);
        QTRY_COMPARE(server.sessions[1].received.size(), 1);
        QCOMPARE(server.sessions[1].received[0].value("setup").toObject().value("sessionResumption").toObject()
                     .value("handle").toString(), QString("handle-1"));

        // A dropped connection comes back too.
        server.reply(R"({"setupComplete":{}})");
        QTRY_VERIFY(gemini.ready());
        server.sessions[1].socket->close(QWebSocketProtocol::CloseCodeGoingAway, "server restarting");
        QTRY_COMPARE(server.sessions.size(), 3);
    }

    void geminiStopsOnARejectedKey() {
        FakeLive server;
        GeminiLiveProvider gemini(server.url());
        QSignalSpy failed(&gemini, &StreamingProvider::failed);
        const QString key = "AIzaSyRejected0000000000000000000000";
        gemini.open("m", key);
        QTRY_COMPARE(server.sessions.size(), 1);
        server.sessions[0].socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                                         "API key not valid. Please pass a valid API key. " + key);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][1].toString(), QString("key"));
        QVERIFY(!failed[0][0].toString().contains(key));
        QVERIFY(failed[0][0].toString().contains("Gemini"));
        QTest::qWait(100);
        QCOMPARE(server.sessions.size(), 1); // no retrying a bad key
    }

    void geminiKeysAreRedacted() {
        QCOMPARE(Provider::redact("bad AIzaSyAbcdefghijklmnop_123-45 here"), QString("bad [key] here"));
        QCOMPARE(Provider::redact("wss://x/y?alt=1&key=secret123"), QString("wss://x/y?alt=1&key=[key]"));
    }

    void backendStreamsWhileLiveOnly() {
        QTemporaryDir dir;
        FakeLive server;
        auto *gemini = new GeminiLiveProvider(server.url());
        auto *microphone = new FakeMicrophone;
        Backend backend(options(dir, {gemini}, microphone));
        QVERIFY(backend.streaming());
        backend.hear(speech(1000)); // before Start: nothing, not even a connection
        QTest::qWait(50);
        QCOMPARE(server.sessions.size(), 0);
        backend.setKey("AIzaSyTestKey000000000000000000000000", false);
        QVERIFY(backend.start());
        QVERIFY(backend.connecting());
        QTRY_COMPARE(server.sessions.size(), 1);
        server.reply(R"({"setupComplete":{}})");
        QTRY_VERIFY(!backend.connecting());
        backend.hear(speech(500));
        QTRY_COMPARE(server.audio().size(), 5);
        server.reply(R"({"serverContent":{"outputTranscription":{"text":"The meeting"}}})");
        QTRY_COMPARE(backend.captionList().count(), 1);
        server.reply(R"({"serverContent":{"outputTranscription":{"text":" starts now."}}})");
        QTRY_COMPARE(backend.captionList().data(backend.captionList().index(0), Captions::TextRole).toString(),
                     QString("The meeting starts now."));
        QCOMPARE(backend.captionList().count(), 1);
        backend.pause();
        QVERIFY(!microphone->active());
        backend.hear(speech(500)); // after Pause: not sent
        QTest::qWait(50);
        QCOMPARE(server.audio().size(), 5);
        backend.shutdown();
        QTRY_COMPARE(server.sessions[0].socket->state(), QAbstractSocket::UnconnectedState);
    }

    void backendShowsWhyAStreamStopped() {
        QTemporaryDir dir;
        FakeLive server;
        auto *gemini = new GeminiLiveProvider(server.url());
        auto *microphone = new FakeMicrophone;
        Backend backend(options(dir, {gemini}, microphone));
        backend.setKey("AIzaSyTestKey000000000000000000000000", false);
        QVERIFY(backend.start());
        QTRY_COMPARE(server.sessions.size(), 1);
        server.sessions[0].socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "Quota exceeded for this model.");
        QTRY_VERIFY(!backend.live());
        QVERIFY(!microphone->active());
        QCOMPARE(backend.problemAction(), QString("network"));
        QVERIFY(backend.problem().contains("limit"));
    }

    // ------------------------------------------------------------ backend

    void backendListensOnlyWhileLive() {
        QTemporaryDir dir;
        auto *provider = new FakeProvider(2);
        auto *microphone = new FakeMicrophone;
        Backend backend(options(dir, {provider}, microphone));
        QVERIFY(!microphone->active());
        backend.hear(room(300) + speech(4000) + room(1000)); // before Start: ignored
        QVERIFY(provider->requests.isEmpty());

        QVERIFY(backend.start());
        QVERIFY(microphone->active());
        QCOMPARE(readFile(dir.filePath("state")).trimmed(), QByteArray("live"));
        backend.hear(room(500) + speech(3000) + room(1000));
        QCOMPARE(provider->requests.size(), 1);

        backend.hear(speech(1500)); // cut short by Pause: still captioned
        backend.pause();
        QVERIFY(!microphone->active());
        QCOMPARE(provider->requests.size(), 2);
        QCOMPARE(readFile(dir.filePath("state")).trimmed(), QByteArray("paused"));

        backend.hear(room(300) + speech(4000) + room(1000)); // after Pause: ignored
        QCOMPARE(provider->requests.size(), 2);

        provider->reply(2, "second");
        provider->reply(1, "first");
        QCOMPARE(backend.captionList().count(), 2);
        QCOMPARE(backend.captionList().data(backend.captionList().index(0), Captions::TextRole).toString(), QString("first"));
    }

    void backendShutdownClosesTheMicrophoneAndForgets() {
        QTemporaryDir dir;
        auto *provider = new FakeProvider(2);
        auto *microphone = new FakeMicrophone;
        {
            Backend backend(options(dir, {provider}, microphone));
            QVERIFY(backend.start());
            backend.hear(room(300) + speech(3000) + room(1000));
            QCOMPARE(provider->requests.size(), 1);
            backend.shutdown();
            QVERIFY(!microphone->active());
            QCOMPARE(provider->cancels, 1);
        }
        QVERIFY(!QFile::exists(dir.filePath("state")));
    }

    void backendWontStartWithoutAKey() {
        QTemporaryDir dir;
        auto *provider = new FakeProvider(2, true);
        auto *microphone = new FakeMicrophone;
        Backend backend(options(dir, {provider}, microphone));
        QCOMPARE(backend.keyState(), QString("missing"));
        QVERIFY(!backend.start());
        QCOMPARE(backend.problemAction(), QString("key"));
        QCOMPARE(microphone->starts, 0);
        backend.setKey("  gsk_fresh  ", false);
        QCOMPARE(backend.keyState(), QString("session"));
        QVERIFY(backend.start());
    }

    void backendShowsEveryProvidersKey() {
        QTemporaryDir dir;
        const QString tool = dir.filePath("secret-tool");
        // A keyring holding a key for the second provider only.
        QVERIFY(writeFile(tool, "#!/bin/sh\ncat > /dev/null\n[ \"$1\" = lookup ] && [ \"$5\" = second ] && { printf gsk_two; exit 0; }\n"
                                "[ \"$1\" = store ] && exit 0\nexit 1\n", true));
        auto *first = new FakeProvider(2, true);
        auto *second = new FakeProvider(2, true);
        second->setObjectName("second");
        auto opts = options(dir, {first, second}, new FakeMicrophone);
        opts.secrets = new Secrets(tool);
        Backend backend(opts);
        QTRY_COMPARE(backend.keyStates().value("second").toString(), QString("saved"));
        QCOMPARE(backend.keyStates().value("fake").toString(), QString("missing"));
        QCOMPARE(backend.providerId(), QString("fake")); // the saved key didn't need choosing it
        backend.setKeyFor("fake", "gsk_one", false);
        QCOMPARE(backend.keyStates().value("fake").toString(), QString("session"));
        backend.forgetKeyFor("second");
        QCOMPARE(backend.keyStates().value("second").toString(), QString("missing"));
        QCOMPARE(backend.keyStates().value("fake").toString(), QString("session"));
    }

    void backendNeverWritesKeysToSettings() {
        QTemporaryDir dir;
        const QString tool = dir.filePath("secret-tool");
        QVERIFY(writeFile(tool, "#!/bin/sh\ncat > /dev/null\nexit 0\n", true));
        auto *provider = new FakeProvider(2, true);
        auto opts = options(dir, {provider}, new FakeMicrophone);
        opts.secrets = new Secrets(tool);
        {
            Backend backend(opts);
            backend.setKey("gsk_neverondisk", true);
            QTRY_COMPARE(backend.keyState(), QString("saved"));
            backend.setChunkSeconds(6);
            backend.setFontSize(48);
        }
        const QByteArray settings = readFile(dir.filePath("relay.conf"));
        QVERIFY(settings.contains("chunkSeconds=6"));
        QVERIFY(!settings.contains("neverondisk"));
        QDirIterator it(dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            QVERIFY(!readFile(it.next()).contains("gsk_neverondisk"));
    }

    void backendPausesWhenTheKeyIsRejected() {
        QTemporaryDir dir;
        auto *provider = new FakeProvider(2, true);
        auto *microphone = new FakeMicrophone;
        Backend backend(options(dir, {provider}, microphone));
        backend.setKey("gsk_x", false);
        QVERIFY(backend.start());
        backend.hear(room(300) + speech(3000) + room(1000));
        provider->reply(1, "Groq rejected the API key.", false, true, true);
        QVERIFY(!backend.live());
        QVERIFY(!microphone->active());
        QCOMPARE(backend.problemAction(), QString("key"));
    }

    void backendRemembersChoicesBetweenRuns() {
        QTemporaryDir dir;
        {
            Backend backend(options(dir, {new GroqProvider, new OpenAIProvider}, new FakeMicrophone));
            backend.setProviderId("openai");
            backend.setModelId("whisper-1");
            backend.setDeviceId("alsa_input.usb-mic");
        }
        Backend backend(options(dir, {new GroqProvider, new OpenAIProvider}, new FakeMicrophone));
        QCOMPARE(backend.providerId(), QString("openai"));
        QCOMPARE(backend.modelId(), QString("whisper-1"));
        QCOMPARE(backend.deviceId(), QString("alsa_input.usb-mic"));
    }

    void captionsAreBounded() {
        Captions captions(3);
        for (int i = 0; i < 5; ++i)
            captions.append(Captions::Caption, QString::number(i));
        QCOMPARE(captions.count(), 3);
        QCOMPARE(captions.data(captions.index(0), Captions::TextRole).toString(), QString("2"));
        QVERIFY(captions.plainText().endsWith("4"));
    }

    // ------------------------------------------------------------ i18n

    void danishCoversEveryString() {
        const QJsonObject da = QJsonDocument::fromJson(readFile(SOURCE_DIR "/i18n/da.json")).object();
        QVERIFY(!da.isEmpty());
        // Plain literals: moc misreads quotes inside raw strings.
        static const QRegularExpression call(
            "\\b(?:qsTr|tr)\\(\\s*\"((?:[^\"\\\\]|\\\\.)*)\"((?:\\s*\"(?:[^\"\\\\]|\\\\.)*\")*)");
        static const QRegularExpression more("\"((?:[^\"\\\\]|\\\\.)*)\"");
        QStringList missing;
        QDirIterator it(SOURCE_DIR, {"*.qml", "*.cpp"}, QDir::Files);
        while (it.hasNext()) {
            const QString source = QString::fromUtf8(readFile(it.next()));
            auto matches = call.globalMatch(source);
            while (matches.hasNext()) {
                const auto m = matches.next();
                QString text = m.captured(1);
                // C++ splits long strings across lines; join the pieces.
                auto pieces = more.globalMatch(m.captured(2));
                while (pieces.hasNext())
                    text += pieces.next().captured(1);
                text.replace("\\\"", "\"").replace("\\n", "\n");
                if (!da.contains(text))
                    missing << text;
            }
        }
        missing.removeDuplicates();
        QVERIFY2(missing.isEmpty(), qPrintable("Missing in da.json:\n" + missing.join('\n')));
    }

    // ------------------------------------------------------------ theme and language

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QVERIFY(writeFile(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\n"));
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        QVERIFY(writeFile(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n", true));
        QVERIFY(writeFile(response, "{\"int\":0}"));
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(writeFile(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(writeFile(dir.path() + "/language", "da\n"));
        QVERIFY(writeFile(dir.path() + "/da.json", R"({"Start": "Start", "Pause": "Pause", "Settings": "Indstillinger"})"));
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("da"));
        QCOMPARE(translator.translate("Main", "Settings"), QString("Indstillinger"));
        QSignalSpy changed(&translator, &Translator::changed);
        QVERIFY(writeFile(dir.path() + "/language", "en\n"));
        QVERIFY(changed.wait(2000));
        QCOMPARE(translator.language(), QString("en"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(writeFile(dir.path() + "/language", "ru\n"));
        QVERIFY(writeFile(dir.path() + "/en.json", R"({"%n waiting": {"one": "%n waiting", "other": "%n waiting"}})"));
        QVERIFY(writeFile(dir.path() + "/ru.json", R"({"%n waiting": {"one": "%n ждёт", "few": "%n ждут", "many": "%n ждут", "other": "%n ждут"}})"));
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n waiting", nullptr, 21), QString("%n ждёт"));
        QCOMPARE(translator.translate("Main", "%n waiting", nullptr, 22), QString("%n ждут"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("si", 1), QString("one"));
    }

private:
    static Backend::Options options(const QTemporaryDir &dir, QList<Provider *> providers, Microphone *microphone) {
        Backend::Options o;
        o.settingsPath = dir.filePath("relay.conf");
        o.stateFile = dir.filePath("state");
        o.providers = providers;
        o.microphone = microphone;
        o.secrets = new Secrets(dir.filePath("no-secret-tool")); // a path that doesn't exist: no keyring
        o.listDevices = false;
        return o;
    }
};

QTEST_MAIN(RelayTests)
#include "relay_tests.moc"
