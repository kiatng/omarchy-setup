#pragma once

#include <QAbstractListModel>
#include <QDateTime>

// The captions on screen, oldest first. They live only in memory and the
// oldest fall away past a limit, so a long meeting doesn't pile up text.
class Captions : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role { TextRole = Qt::UserRole + 1, LanguageRole, TimeRole, KindRole, LatencyRole };
    enum Kind { Caption, Notice, Error };
    Q_ENUM(Kind)

    explicit Captions(int limit = 400, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return int(m_rows.size()); }
    void append(Kind kind, const QString &text, const QString &language = {}, int latencyMs = 0);
    // The caption still being spoken: shown at once and replaced as more
    // comes in, until commitLive() leaves it as it is.
    void setLive(const QString &text, const QString &language);
    void commitLive() { m_live = -1; }
    bool hasLive() const { return m_live >= 0; }
    Q_INVOKABLE void clear();
    // Every caption as plain text, for an explicit copy.
    QString plainText() const;

signals:
    void countChanged();

private:
    struct Row { Kind kind; QString text, language; QDateTime time; int latencyMs; };
    QList<Row> m_rows;
    int m_limit;
    int m_live = -1;
};
