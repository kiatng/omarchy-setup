#include "pipeline.h"

#include "audio.h"

Pipeline::Pipeline(QObject *parent) : QObject(parent) {}

void Pipeline::setProvider(Provider *provider, const QString &model, const QString &key) {
    if (m_provider != provider || m_model != model || m_key != key)
        cancel();
    if (m_connection)
        disconnect(m_connection);
    m_provider = provider;
    m_model = model;
    m_key = key;
    if (provider)
        m_connection = connect(provider, &Provider::finished, this, &Pipeline::received);
}

void Pipeline::submit(const AudioChunk &chunk) {
    TranslationRequest request;
    request.sequence = m_nextSequence++;
    request.samples = chunk.samples;
    m_waiting.enqueue(request);
    while (m_waiting.size() > m_maxWaiting) {
        const TranslationRequest old = m_waiting.dequeue();
        m_dropped.insert(old.sequence);
        emit skipped(old.samples.size() / double(Audio::SampleRate));
    }
    pump();
    deliver();
    emit activityChanged();
}

void Pipeline::cancel() {
    if (m_provider)
        m_provider->cancelAll();
    m_waiting.clear();
    m_inFlight.clear();
    m_dropped.clear();
    m_done.clear();
    m_nextDelivery = m_nextSequence;
    m_lastCaption.clear();
    emit activityChanged();
}

void Pipeline::pump() {
    if (!m_provider)
        return;
    while (!m_waiting.isEmpty() && m_inFlight.size() < m_provider->maxConcurrent()) {
        TranslationRequest request = m_waiting.dequeue();
        request.context = m_lastCaption;
        m_inFlight.insert(request.sequence);
        m_provider->translate(request, m_model, m_key);
    }
}

void Pipeline::received(const Translation &translation) {
    // A reply for a chunk cancelled since is stale.
    if (!m_inFlight.remove(translation.sequence))
        return;
    m_done.insert(translation.sequence, translation);
    deliver();
    pump();
    emit activityChanged();
}

void Pipeline::deliver() {
    while (true) {
        if (m_dropped.remove(m_nextDelivery)) {
            ++m_nextDelivery;
            continue;
        }
        auto it = m_done.find(m_nextDelivery);
        if (it == m_done.end())
            return;
        const Translation translation = *it;
        m_done.erase(it);
        ++m_nextDelivery;
        if (translation.ok && !translation.text.isEmpty())
            m_lastCaption = translation.text;
        emit result(translation);
    }
}
