#pragma once

#include <QList>
#include <QVector>
#include <optional>

// A stretch of speech ready to translate.
struct AudioChunk {
    QVector<qint16> samples; // 16 kHz mono
    int speechMs = 0;        // how much of it sounded like speech

    double seconds() const { return samples.size() / 16000.0; }
};

// Cuts a continuous 16 kHz stream into chunks at the pauses between phrases,
// so a sentence is rarely split mid-word and each request carries whole
// thoughts for the translator. Silence never leaves the machine: stretches
// with too little speech are dropped here, and leading and trailing silence
// is trimmed from the chunks that are kept.
class Segmenter {
public:
    struct Config {
        int minMs = 2500;      // shortest chunk cut at a pause
        int maxMs = 8000;      // longest chunk; cut at the quietest moment near it
        int pauseMs = 600;     // silence that ends a phrase
        int minSpeechMs = 350; // less speech than this is noise, not a phrase
        int keepSilenceMs = 250; // silence kept on either side of the speech
    };

    Segmenter() : Segmenter(Config()) {}
    explicit Segmenter(const Config &config);

    void setConfig(const Config &config) { m_config = config; }
    const Config &config() const { return m_config; }

    // Feeds samples and returns the chunks they completed, in order.
    QList<AudioChunk> push(const QVector<qint16> &samples);
    // Ends the stream (Pause): returns what's left if it holds speech.
    std::optional<AudioChunk> flush();
    void reset();

    bool inSpeech() const { return m_speechFrames > 0; }
    double noiseFloorDb() const { return m_floorDb; }

    static constexpr int FrameMs = 20;
    static constexpr int FrameSamples = 16000 * FrameMs / 1000;

private:
    void addFrame(const qint16 *frame);
    std::optional<AudioChunk> cut(int frames);

    Config m_config;
    QVector<qint16> m_partial;   // samples short of a whole frame
    QVector<qint16> m_buffer;    // whole frames of the current chunk
    QVector<float> m_energy;     // dBFS per frame in m_buffer
    QVector<bool> m_speech;      // speech flag per frame in m_buffer
    int m_speechFrames = 0;      // speech frames in m_buffer
    int m_silentRun = 0;         // trailing silent frames
    float m_floorDb = -45.f;     // the room's background level
    QVector<float> m_history;    // recent frame levels, for the floor
    static constexpr int HistoryFrames = 3000 / FrameMs;
    QList<AudioChunk> m_ready;
};
