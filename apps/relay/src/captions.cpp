#include "captions.h"

Captions::Captions(int limit, QObject *parent) : QAbstractListModel(parent), m_limit(qMax(1, limit)) {}

int Captions::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant Captions::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case TextRole: return row.text;
    case LanguageRole: return row.language;
    case TimeRole: return row.time;
    case KindRole: return int(row.kind);
    case LatencyRole: return row.latencyMs;
    default: return {};
    }
}

QHash<int, QByteArray> Captions::roleNames() const {
    return {{TextRole, "text"}, {LanguageRole, "language"}, {TimeRole, "time"},
            {KindRole, "kind"}, {LatencyRole, "latencyMs"}};
}

void Captions::append(Kind kind, const QString &text, const QString &language, int latencyMs) {
    // Anything else shown ends the caption being spoken, so it isn't
    // updated above a later line.
    m_live = -1;
    if (m_rows.size() >= m_limit) {
        const int drop = int(m_rows.size()) - m_limit + 1;
        beginRemoveRows({}, 0, drop - 1);
        m_rows.remove(0, drop);
        endRemoveRows();
    }
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size()));
    m_rows.append({kind, text, language, QDateTime::currentDateTime(), latencyMs});
    endInsertRows();
    emit countChanged();
}

void Captions::setLive(const QString &text, const QString &language) {
    if (m_live < 0 || m_live >= m_rows.size()) {
        append(Caption, text, language);
        m_live = int(m_rows.size()) - 1;
        return;
    }
    Row &row = m_rows[m_live];
    if (row.text == text && row.language == language)
        return;
    row.text = text;
    row.language = language;
    emit dataChanged(index(m_live), index(m_live), {TextRole, LanguageRole});
}

void Captions::clear() {
    m_live = -1;
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

QString Captions::plainText() const {
    QStringList lines;
    for (const Row &row : m_rows)
        if (row.kind == Caption)
            lines << row.time.toString("HH:mm:ss") + "  " + (row.language.isEmpty() ? QString() : "[" + row.language + "] ") + row.text;
    return lines.join('\n');
}
