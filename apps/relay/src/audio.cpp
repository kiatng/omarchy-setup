#include "audio.h"

#include <QtEndian>
#include <cmath>

namespace Audio {

Converter::Converter(const QAudioFormat &input) {
    setInput(input);
}

void Converter::setInput(const QAudioFormat &input) {
    m_input = input;
    const int rate = input.isValid() ? input.sampleRate() : SampleRate;
    m_step = double(rate) / SampleRate;
    reset();
}

void Converter::reset() {
    m_position = 0.0;
    m_pending.clear();
}

QVector<qint16> Converter::convert(const char *data, qsizetype bytes) {
    if (!m_input.isValid() || bytes <= 0)
        return {};

    // Mix every frame down to one float channel first, then resample.
    const int channels = qMax(1, m_input.channelCount());
    const int bytesPerSample = m_input.bytesPerSample();
    const int frameBytes = channels * bytesPerSample;
    if (bytesPerSample <= 0 || frameBytes <= 0)
        return {};
    const qsizetype frames = bytes / frameBytes;
    m_pending.reserve(m_pending.size() + frames);
    for (qsizetype f = 0; f < frames; ++f) {
        const char *frame = data + f * frameBytes;
        float sum = 0.f;
        for (int c = 0; c < channels; ++c)
            sum += m_input.normalizedSampleValue(frame + c * bytesPerSample);
        m_pending.append(sum / channels);
    }

    // Linear interpolation is plenty for speech: Whisper's own front end
    // discards everything above 8 kHz anyway. Downsampling from 48 kHz first
    // averages each output's span of input, a box filter that keeps the
    // worst of the aliasing out.
    QVector<qint16> out;
    const double span = m_step > 1.0 ? m_step : 0.0;
    while (m_position + 1.0 + span < m_pending.size()) {
        float value;
        if (span > 0.0) {
            const int from = int(m_position), to = qMin<int>(int(m_position + span), m_pending.size() - 1);
            float sum = 0.f;
            for (int i = from; i <= to; ++i)
                sum += m_pending.at(i);
            value = sum / (to - from + 1);
        } else {
            const int index = int(m_position);
            const float t = float(m_position - index);
            value = m_pending.at(index) * (1.f - t) + m_pending.at(index + 1) * t;
        }
        out.append(qint16(std::lround(qBound(-1.f, value, 1.f) * 32767.f)));
        m_position += m_step;
    }

    // Keep only the input the next buffer still needs.
    const int consumed = qMax(0, int(m_position));
    m_pending.remove(0, qMin<qsizetype>(consumed, m_pending.size()));
    m_position -= consumed;
    return out;
}

double Converter::level(const QVector<qint16> &samples) {
    if (samples.isEmpty())
        return 0.0;
    double sum = 0.0;
    for (qint16 s : samples)
        sum += double(s) * s;
    return std::sqrt(sum / samples.size()) / 32768.0;
}

QByteArray encodeWav(const QVector<qint16> &samples, int sampleRate) {
    const quint32 dataBytes = quint32(samples.size() * 2);
    QByteArray wav;
    wav.reserve(44 + int(dataBytes));
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); wav.append(b, 4); };
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); wav.append(b, 2); };
    wav.append("RIFF");
    u32(36 + dataBytes);
    wav.append("WAVEfmt ");
    u32(16);              // fmt chunk size
    u16(1);               // PCM
    u16(1);               // mono
    u32(quint32(sampleRate));
    u32(quint32(sampleRate) * 2);
    u16(2);               // block align
    u16(16);              // bits per sample
    wav.append("data");
    u32(dataBytes);
    for (qint16 s : samples)
        u16(quint16(s));
    return wav;
}

} // namespace Audio
