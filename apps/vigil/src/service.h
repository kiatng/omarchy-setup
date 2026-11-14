#pragma once
#include <QDBusUnixFileDescriptor>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalServer>
#include <QMap>
#include <QObject>
#include <QTimer>
struct Task {
  QString name;
  qint64 expires;
  qint64 pid = 0;
  QString start;
};
class Service : public QObject {
  Q_OBJECT
public:
  explicit Service(QObject *p = nullptr);
  bool start();
  QJsonObject request(const QJsonObject &r);
private slots:
  void tick();
  void preparing(bool sleeping);

protected:
  QLocalServer server;
  QTimer timer;
  QElapsedTimer clock;
  QMap<QString, Task> tasks;
  QDBusUnixFileDescriptor inhibitor;
  bool manual = false, allowed = false, ac = true, lid = false, marker = false,
       suspended = false;
  double battery = -1, minutes = -1;
  int reserve = 15;
  qint64 deadline = 0;
  QString state = "normal", reason = "no-holds", error, lastEvent;
  QString runtime, markerPath;
  qint64 markerStamp = 0;
  virtual void hardware();
  virtual bool acquire();
  virtual bool held() const { return inhibitor.isValid(); }
  virtual void release() { inhibitor = {}; }
  void publish();
  void setMarker(bool on);
  virtual void suspend();
  QJsonObject status() const;
  virtual void notify(const QString &message);
};
QString socketPath();
QJsonObject callService(const QJsonObject &request);
QString processStart(qint64 pid);
