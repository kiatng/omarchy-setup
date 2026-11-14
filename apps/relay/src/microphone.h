#pragma once

#include <QAudioSource>
#include <QObject>
#include <QPointer>

#include "audio.h"

// The microphone, open only between start() and stop(). Delivers 16 kHz
// mono samples whatever the device's own format. Virtual so tests can stand
// in a fake and check exactly when capture runs.
class Microphone : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~Microphone() override;

    // An empty id is the system default input.
    virtual bool start(const QByteArray &deviceId, QString *error);
    virtual void stop();
    virtual bool active() const { return m_source != nullptr; }

signals:
    void samples(const QVector<qint16> &samples);
    void failed(const QString &message);

private:
    void read();

    QAudioSource *m_source = nullptr;
    QPointer<QIODevice> m_io;
    Audio::Converter m_converter;
};
