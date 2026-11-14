#pragma once
#include <QObject>
#include <QTimer>
#include <QVariantMap>
class Backend : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
public:
  Backend();
  QVariantMap status() const { return data; }
  QString error() const { return message; }
  Q_INVOKABLE void action(QString command, int value = 0);
  void refresh();
signals:
  void changed();

private:
  QVariantMap data;
  QString message;
  QTimer timer;
};
