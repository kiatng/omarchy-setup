#include "service.h"
#include "policy.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <memory>
#include <unistd.h>
static QByteArray read(const QString &p) {
  QFile f(p);
  return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed() : QByteArray();
}
static bool save(const QString &p, const QByteArray &v) {
  QSaveFile f(p);
  return f.open(QIODevice::WriteOnly) && f.write(v) == v.size() && f.commit();
}
QString socketPath() {
  return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
         "/vigil/control";
}
QString processStart(qint64 pid) {
  const auto s = read(QString("/proc/%1/stat").arg(pid));
  const auto parts = s.mid(s.lastIndexOf(')') + 2).split(' ');
  return parts.size() > 19 && parts[0] != "Z" ? QString(parts[19]) : QString();
}
QJsonObject callService(const QJsonObject &r) {
  QLocalSocket s;
  s.connectToServer(socketPath());
  if (!s.waitForConnected(1500))
    return {{"ok", false},
            {"error", "Vigil service is unavailable. Start vigil.service."}};
  s.write(QJsonDocument(r).toJson(QJsonDocument::Compact) + '\n');
  s.flush();
  QByteArray data;
  QElapsedTimer t;
  t.start();
  while (!data.contains('\n') && t.elapsed() < 4000) {
    if (s.bytesAvailable() || s.waitForReadyRead(500))
      data += s.readAll();
  }
  auto o = QJsonDocument::fromJson(data).object();
  return o.isEmpty()
             ? QJsonObject{{"ok", false},
                           {"error", "Vigil did not return a valid response."}}
             : o;
}
Service::Service(QObject *p) : QObject(p) {
  runtime = QFileInfo(socketPath()).absolutePath();
  markerPath = QDir::homePath() + "/.local/state/omarchy/indicators/stay-awake";
  clock.start();
  timer.setInterval(1000);
  connect(&timer, &QTimer::timeout, this, &Service::tick);
}
bool Service::start() {
  QDir().mkpath(runtime);
  QFile::setPermissions(runtime,
                        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  QLocalSocket probe;
  probe.connectToServer(socketPath());
  if (probe.waitForConnected(200))
    return false;
  QLocalServer::removeServer(socketPath());
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if (!server.listen(socketPath()))
    return false;
  auto settings =
      QJsonDocument::fromJson(read(QStandardPaths::writableLocation(
                                       QStandardPaths::AppConfigLocation) +
                                   "/settings.json"))
          .object();
  reserve = qBound(5, settings.value("reserve").toInt(15), 50);
  connect(&server, &QLocalServer::newConnection, this, [this] {
    while (auto s = server.nextPendingConnection()) {
      auto buffer = std::make_shared<QByteArray>();
      connect(s, &QLocalSocket::readyRead, this, [this, s, buffer] {
        *buffer += s->readAll();
        if (buffer->size() > 16384) {
          s->disconnectFromServer();
          return;
        }
        if (!buffer->contains('\n'))
          return;
        auto r = QJsonDocument::fromJson(*buffer).object();
        s->write(QJsonDocument(request(r)).toJson(QJsonDocument::Compact) +
                 '\n');
        s->disconnectFromServer();
      });
      connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
      QTimer::singleShot(5000, s, [s] { s->disconnectFromServer(); });
    }
  });
  QDBusConnection::systemBus().connect(
      "org.freedesktop.login1", "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager", "PrepareForSleep", this,
      SLOT(preparing(bool)));
  setMarker(false);
  tick();
  timer.start();
  return true;
}
void Service::hardware() {
  QDBusInterface props("org.freedesktop.login1", "/org/freedesktop/login1",
                       "org.freedesktop.DBus.Properties",
                       QDBusConnection::systemBus());
  QDBusReply<QDBusVariant> l =
      props.call("Get", "org.freedesktop.login1.Manager", "LidClosed");
  if (l.isValid())
    lid = l.value().variant().toBool();
  ac = false;
  double sum = 0;
  int count = 0, packs = 0;
  double energy = 0, power = 0;
  QDir dir("/sys/class/power_supply");
  for (const auto &n : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    auto path = dir.filePath(n) + "/";
    auto type = read(path + "type");
    if (type == "Battery" && read(path + "scope") != "Device" &&
        read(path + "present") != "0") {
      ++packs;
      bool ok = false;
      double cap = read(path + "capacity").toDouble(&ok);
      if (ok) {
        sum += cap;
        count++;
      }
      energy += read(path + "energy_now").toDouble();
      power += read(path + "power_now").toDouble();
    } else if (read(path + "online") == "1")
      ac = true;
  }
  battery = count ? sum / count : -1;
  if (!packs)
    ac = true;
  minutes = (!ac && power > 0) ? energy / power * 60 : -1;
}
bool Service::acquire() {
  if (held())
    return true;
  QDBusInterface login("org.freedesktop.login1", "/org/freedesktop/login1",
                       "org.freedesktop.login1.Manager",
                       QDBusConnection::systemBus());
  QDBusReply<QDBusUnixFileDescriptor> r =
      login.call("Inhibit", "sleep:handle-lid-switch", "Vigil",
                 "Local work is running", "block");
  if (!r.isValid()) {
    error = r.error().message();
    return false;
  }
  inhibitor = r.value();
  error.clear();
  return true;
}
void Service::setMarker(bool on) {
  QDir().mkpath(QFileInfo(markerPath).absolutePath());
  bool ok = on ? save(markerPath, "vigil\n")
               : (!QFile::exists(markerPath) || QFile::remove(markerPath));
  if (ok) {
    marker = on;
    markerStamp = QFileInfo(markerPath).lastModified().toMSecsSinceEpoch();
  } else
    error = "Cannot synchronize Omarchy's idle switch.";
}
void Service::notify(const QString &message) {
  QProcess::startDetached("notify-send", {"-a", "Vigil", "Vigil", message});
}
QJsonObject Service::status() const {
  QJsonArray ts;
  for (auto it = tasks.begin(); it != tasks.end(); ++it)
    ts.append(QJsonObject{
        {"name", it->name},
        {"supervised", it->pid > 0},
        {"expires_in",
         int(qMax(qint64(0), it->expires - clock.elapsed()) / 1000)}});
  return {
      {"ok", true},
      {"version", 1},
      {"updated_ms", double(QDateTime::currentMSecsSinceEpoch())},
      {"state", state},
      {"reason", reason},
      {"manual", manual},
      {"allow_tasks", allowed},
      {"inhibited", held()},
      {"external_power", ac},
      {"battery", battery},
      {"minutes_remaining", minutes},
      {"lid_closed", lid},
      {"reserve", reserve},
      {"countdown",
       deadline ? int(qMax(qint64(0), deadline - clock.elapsed()) / 1000) : 0},
      {"tasks", ts},
      {"error", error},
      {"last_event", lastEvent}};
}
void Service::publish() {
  save(runtime + "/status.json",
       QJsonDocument(status()).toJson(QJsonDocument::Compact));
}
void Service::preparing(bool sleeping) {
  suspended = sleeping;
  if (sleeping) {
    manual = false;
    allowed = false;
    tasks.clear();
    deadline = 0;
    release();
    setMarker(false);
  } else {
    lastEvent = "resumed";
    tick();
  }
}
void Service::suspend() {
  state = "suspending";
  publish();
  release();
  manual = false;
  allowed = false;
  tasks.clear();
  deadline = 0;
  setMarker(false);
  QDBusInterface login("org.freedesktop.login1", "/org/freedesktop/login1",
                       "org.freedesktop.login1.Manager",
                       QDBusConnection::systemBus());
  QDBusMessage r = login.call("Suspend", false);
  if (r.type() == QDBusMessage::ErrorMessage) {
    error = r.errorMessage();
    state = "error";
    reason = "suspend-failed";
    notify(tr("Sleep failed. Open Vigil for details."));
  }
  publish();
}
void Service::tick() {
  if (suspended)
    return;
  bool externalMarker = QFile::exists(markerPath);
  const auto stamp = QFileInfo(markerPath).lastModified().toMSecsSinceEpoch();
  if (externalMarker != marker || (externalMarker && stamp != markerStamp)) {
    manual = externalMarker;
    allowed = externalMarker;
    if (!externalMarker)
      tasks.clear();
    marker = externalMarker;
    markerStamp = stamp;
    lastEvent = "omarchy-toggle";
  }
  hardware();
  for (auto it = tasks.begin(); it != tasks.end();) {
    if ((it->pid > 0 && processStart(it->pid) != it->start) ||
        (it->pid == 0 && it->expires <= clock.elapsed())) {
      lastEvent = it->pid ? "process-exited" : "task-connection-lost";
      it = tasks.erase(it);
    } else
      ++it;
  }
  bool requested = manual || !tasks.isEmpty();
  auto d = decide({requested, ac, lid, held(), battery, reserve});
  QString old = state;
  if (!d.pending)
    deadline = 0;
  if (d.inhibit && !acquire()) {
    state = "error";
    reason = "inhibitor-failed";
    deadline = 0;
    publish();
    return;
  }
  if (!d.inhibit)
    release();
  state = d.state;
  reason = d.reason;
  if (reason == "battery-unavailable")
    error = "Battery reading unavailable; sleep protection is not active.";
  if (d.pending && !deadline) {
    deadline = clock.elapsed() + 60000;
    notify(tr("Sleep in 60 seconds. Connect power or open Vigil to cancel."));
  }
  if (state == "battery-warning" && old != state)
    notify(tr("Battery is low. Work continues until the reserve."));
  if (marker != requested)
    setMarker(requested);
  if (deadline && clock.elapsed() >= deadline) {
    suspend();
    return;
  }
  publish();
}
QJsonObject Service::request(const QJsonObject &r) {
  QString cmd = r.value("command").toString();
  auto fail = [](const QString &s) {
    return QJsonObject{{"ok", false}, {"error", s}};
  };
  if (cmd == "status")
    return status();
  if (cmd == "on") {
    manual = true;
    allowed = true;
    error.clear();
  } else if (cmd == "off") {
    manual = false;
    allowed = false;
    tasks.clear();
    error.clear();
  } else if (cmd == "auto") {
    manual = false;
    allowed = true;
    error.clear();
  } else if (cmd == "toggle") {
    bool on = manual || !tasks.isEmpty();
    manual = !on;
    allowed = !on;
    if (on)
      tasks.clear();
    error.clear();
  } else if (cmd == "reserve") {
    int value = r.value("value").toInt();
    if (value < 5 || value > 50)
      return fail("Reserve must be between 5 and 50.");
    auto path =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    if (!save(path + "/settings.json",
              QJsonDocument(QJsonObject{{"reserve", value}}).toJson()))
      return fail("Could not save reserve.");
    reserve = value;
  } else if (cmd == "task-begin") {
    hardware();
    if (!ac && (battery < 0 || battery <= reserve))
      return fail("Battery is at reserve or unavailable. Connect power before "
                  "starting a task.");
    if (!allowed)
      return fail("Task requests are disabled. The user must enable Allow task "
                  "requests in Vigil.");
    QString name = r.value("name").toString().trimmed();
    if (name.isEmpty() || name.size() > 120 || tasks.size() >= 128)
      return fail("Provide a name up to 120 characters; at most 128 tasks are "
                  "allowed.");
    qint64 pid = qint64(r.value("pid").toDouble());
    QString start = pid ? processStart(pid) : QString();
    if (pid && (start.isEmpty() ||
                QFileInfo(QString("/proc/%1").arg(pid)).ownerId() != getuid()))
      return fail("Process is unavailable or belongs to another user.");
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    tasks[id] = {name, clock.elapsed() + 900000, pid, start};
    tick();
    if (!held()) {
      tasks.remove(id);
      return fail(error.isEmpty() ? "Could not protect task." : error);
    }
    return {{"ok", true}, {"id", id}, {"lease_seconds", 900}};
  } else if (cmd == "task-finish" || cmd == "task-renew") {
    QString id = r.value("id").toString();
    if (!tasks.contains(id))
      return fail("Unknown or expired task token.");
    if (cmd == "task-finish")
      tasks.remove(id);
    else
      tasks[id].expires = clock.elapsed() + 900000;
  } else if (cmd == "sleep") {
    suspend();
    auto response = status();
    if (!error.isEmpty())
      response["ok"] = false;
    return response;
  } else if (cmd == "cancel") {
    if (reason == "battery-reserve") {
      manual = false;
      allowed = false;
      tasks.clear();
      release();
    } else {
      manual = true;
      allowed = true;
    }
    deadline = 0;
  } else
    return fail("Unknown command. Use vigil --help.");
  setMarker(manual || !tasks.isEmpty());
  tick();
  auto response = status();
  if (!error.isEmpty())
    response["ok"] = false;
  return response;
}
