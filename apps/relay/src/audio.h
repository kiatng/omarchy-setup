#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QVector>

// Everything Relay sends or hands to a local model is 16 kHz mono 16-bit PCM,
// the rate Whisper works at internally, so the smallest upload carries all the
// detail the model can use.
namespace Audio {

constexpr int SampleRate = 16000;

// Converts whatever the microphone delivers (any rate, channel count, and
// sample format Qt Multimedia supports) to 16 kHz mono int16. It keeps the
// resampler's position between buffers, so feeding a stream in pieces gives
// the same samples as feeding it whole.
class Converter {
public:
    explicit Converter(const QAudioFormat &input = {});

    void setInput(const QAudioFormat &input);
    QVector<qint16> convert(const char *data, qsizetype bytes);
    void reset();

    // Root mean square of the samples on a 0..1 scale.
    static double level(const QVector<qint16> &samples);

private:
    QAudioFormat m_input;
    double m_step = 1.0;      // input samples per output sample
    double m_position = 0.0;  // fractional read position into m_pending
    QVector<float> m_pending; // mono input samples not yet consumed
};

// A WAV file of 16-bit mono PCM at the given rate.
QByteArray encodeWav(const QVector<qint16> &samples, int sampleRate = SampleRate);

} // namespace Audio
