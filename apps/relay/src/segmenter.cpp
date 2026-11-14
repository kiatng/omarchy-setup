#include "segmenter.h"

#include <algorithm>
#include <cmath>

Segmenter::Segmenter(const Config &config) : m_config(config) {}

void Segmenter::reset() {
    m_partial.clear();
    m_buffer.clear();
    m_energy.clear();
    m_speech.clear();
    m_speechFrames = 0;
    m_silentRun = 0;
    m_ready.clear();
    m_history.clear();
}

QList<AudioChunk> Segmenter::push(const QVector<qint16> &samples) {
    m_partial += samples;
    qsizetype offset = 0;
    while (m_partial.size() - offset >= FrameSamples) {
        addFrame(m_partial.constData() + offset);
        offset += FrameSamples;
    }
    m_partial.remove(0, offset);
    QList<AudioChunk> ready;
    ready.swap(m_ready);
    return ready;
}

void Segmenter::addFrame(const qint16 *frame) {
    double sum = 0.0;
    for (int i = 0; i < FrameSamples; ++i)
        sum += double(frame[i]) * frame[i];
    const double rms = std::sqrt(sum / FrameSamples) / 32768.0;
    const float db = float(20.0 * std::log10(qMax(rms, 1e-6)));

    // Speech stands out from the room by a margin. The room's level is a low
    // percentile of the last three seconds: speech always dips between
    // syllables, so the floor finds the fan or air conditioner underneath
    // it within seconds and never climbs into a long speech. The first
    // 200 ms only listen to the room, so a fan isn't mistaken for a voice;
    // that audio is kept as lead-in, so a first word isn't lost.
    m_history.append(db);
    if (m_history.size() > HistoryFrames)
        m_history.removeFirst();
    bool speech = false;
    if (m_history.size() >= 10) {
        QVector<float> sorted = m_history;
        const auto at = sorted.begin() + sorted.size() / 10;
        std::nth_element(sorted.begin(), at, sorted.end());
        m_floorDb = qMax(*at, -70.f);
        speech = db > qMax(m_floorDb + 9.f, -52.f);
    }

    m_buffer.append(QVector<qint16>(frame, frame + FrameSamples));
    m_energy.append(db);
    m_speech.append(speech);
    if (speech) {
        ++m_speechFrames;
        m_silentRun = 0;
    } else {
        ++m_silentRun;
    }

    const int frames = m_energy.size();
    const int keepFrames = m_config.keepSilenceMs / FrameMs;

    // Before anyone speaks, only a little lead-in is kept.
    if (m_speechFrames == 0) {
        if (frames > keepFrames) {
            const int drop = frames - keepFrames;
            m_buffer.remove(0, qsizetype(drop) * FrameSamples);
            m_energy.remove(0, drop);
            m_speech.remove(0, drop);
        }
        return;
    }

    const int lengthMs = frames * FrameMs;
    if (lengthMs >= m_config.minMs && m_silentRun * FrameMs >= m_config.pauseMs) {
        if (auto chunk = cut(frames - m_silentRun + keepFrames))
            m_ready.append(*chunk);
        return;
    }
    if (lengthMs >= m_config.maxMs) {
        // No pause came: cut at the quietest frame of the last 1.5 seconds so
        // a word is less likely to be split across two requests.
        const int window = qMin(frames - 1, 1500 / FrameMs);
        int best = frames;
        float quietest = 1e9f;
        for (int i = frames - window; i < frames; ++i) {
            if (m_energy.at(i) < quietest) {
                quietest = m_energy.at(i);
                best = i + 1;
            }
        }
        if (auto chunk = cut(best))
            m_ready.append(*chunk);
    }
}

// Takes the first `frames` frames as a chunk; the rest starts the next one.
std::optional<AudioChunk> Segmenter::cut(int frames) {
    frames = qBound(0, frames, int(m_energy.size()));
    int speechFrames = 0;
    for (int i = 0; i < frames; ++i)
        speechFrames += m_speech.at(i) ? 1 : 0;

    AudioChunk chunk;
    chunk.samples = m_buffer.first(qsizetype(frames) * FrameSamples);
    chunk.speechMs = speechFrames * FrameMs;

    m_buffer.remove(0, qsizetype(frames) * FrameSamples);
    m_energy.remove(0, frames);
    m_speech.remove(0, frames);
    m_speechFrames -= speechFrames;
    m_silentRun = 0;
    for (int i = m_speech.size() - 1; i >= 0 && !m_speech.at(i); --i)
        ++m_silentRun;

    if (chunk.speechMs < m_config.minSpeechMs)
        return std::nullopt;
    return chunk;
}

std::optional<AudioChunk> Segmenter::flush() {
    // Trailing silence beyond the lead-out isn't worth sending.
    const int keepFrames = m_config.keepSilenceMs / FrameMs;
    const int frames = int(m_energy.size()) - qMax(0, m_silentRun - keepFrames);
    auto chunk = m_speechFrames > 0 ? cut(frames) : std::nullopt;
    reset();
    return chunk;
}
