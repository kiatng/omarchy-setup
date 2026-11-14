#include "microphone.h"

#include <QMediaDevices>

Microphone::~Microphone() {
    // Closing the app always closes the microphone.
    Microphone::stop();
}

bool Microphone::start(const QByteArray &deviceId, QString *error) {
    stop();
    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!deviceId.isEmpty()) {
        device = {};
        for (const auto &candidate : QMediaDevices::audioInputs())
            if (candidate.id() == deviceId)
                device = candidate;
    }
    if (device.isNull()) {
        if (error)
            *error = deviceId.isEmpty() ? tr("No microphone is connected.")
                                        : tr("The selected microphone isn't connected. Pick another one.");
        return false;
    }

    // Take the device's own format and convert here, rather than asking for
    // 16 kHz and depending on the audio server to resample.
    QAudioFormat format = device.preferredFormat();
    if (!format.isValid()) {
        format.setSampleRate(Audio::SampleRate);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
    }
    m_converter.setInput(format);
    m_source = new QAudioSource(device, format, this);
    m_source->setBufferSize(format.bytesForDuration(200000));
    connect(m_source, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (m_source && state == QAudio::StoppedState && m_source->error() != QAudio::NoError) {
            const auto code = m_source->error();
            stop();
            emit failed(code == QAudio::OpenError ? tr("The microphone couldn't be opened; another app may hold it.")
                                                  : tr("The microphone stopped delivering audio."));
        }
    });
    m_io = m_source->start();
    if (!m_io || m_source->error() != QAudio::NoError) {
        stop();
        if (error)
            *error = tr("The microphone couldn't be opened; another app may hold it.");
        return false;
    }
    connect(m_io, &QIODevice::readyRead, this, &Microphone::read);
    return true;
}

void Microphone::read() {
    if (!m_io)
        return;
    const QByteArray data = m_io->readAll();
    const auto converted = m_converter.convert(data.constData(), data.size());
    if (!converted.isEmpty())
        emit samples(converted);
}

void Microphone::stop() {
    if (!m_source)
        return;
    QAudioSource *source = m_source;
    m_source = nullptr;
    source->disconnect(this);
    source->stop();
    source->deleteLater();
    m_io = nullptr;
    m_converter.reset();
}
