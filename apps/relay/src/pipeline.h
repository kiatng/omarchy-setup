#pragma once

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>

#include "providers.h"
#include "segmenter.h"

// Sends chunks to a provider a few at a time and hands back the results in
// the order the words were spoken, whatever order the replies arrive in.
// It holds only a little audio: when the provider falls behind, the oldest
// waiting chunk is dropped (and reported) instead of captions trailing
// further and further behind the room.
class Pipeline : public QObject {
    Q_OBJECT
public:
    explicit Pipeline(QObject *parent = nullptr);

    void setProvider(Provider *provider, const QString &model, const QString &key);
    void setMaxWaiting(int chunks) { m_maxWaiting = qMax(1, chunks); }

    void submit(const AudioChunk &chunk);
    // Forgets everything waiting or in flight; nothing more is emitted for it.
    void cancel();

    int inFlight() const { return m_inFlight.size(); }
    int waiting() const { return m_waiting.size(); }
    bool idle() const { return m_inFlight.isEmpty() && m_waiting.isEmpty(); }

signals:
    void result(const Translation &translation); // in spoken order
    void skipped(double seconds);                // audio dropped to keep up
    void activityChanged();

private:
    void pump();
    void received(const Translation &translation);
    void deliver();

    QPointer<Provider> m_provider;
    QString m_model, m_key;
    int m_maxWaiting = 3;
    quint64 m_nextSequence = 1;   // given to the next chunk submitted
    quint64 m_nextDelivery = 1;   // the result to emit next
    QQueue<TranslationRequest> m_waiting;
    QSet<quint64> m_inFlight;
    QSet<quint64> m_dropped;
    QMap<quint64, Translation> m_done;
    QString m_lastCaption;
    QMetaObject::Connection m_connection;
};
