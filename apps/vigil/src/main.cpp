#include "backend.h"
#include "service.h"
#include "theme.h"
#include "translator.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <unistd.h>
static volatile sig_atomic_t interrupted = 0;
static void signalHandler(int s) { interrupted = s; }
static bool writeFile(QString path, QByteArray text) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(text) == text.size() &&
         f.commit();
}
static int result(QJsonObject o) {
  bool ok = o.value("ok").toBool();
  QTextStream(ok ? stdout : stderr)
      << QJsonDocument(o).toJson(QJsonDocument::Compact) << Qt::endl;
  return ok ? 0 : 1;
}
int main(int argc, char **argv) {
  QString cmd = argc > 1 ? QString::fromLocal8Bit(argv[1]) : "open";
  if (cmd != "open") {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "");
  }
  QGuiApplication app(argc, argv);
  app.setApplicationName("vigil");
  app.setApplicationVersion("0.1.0");
  app.setDesktopFileName("vigil");
  auto args = app.arguments().mid(2);
  if (cmd == "daemon") {
    Service service;
    if (!service.start())
      return 1;
    return app.exec();
  }
  if (cmd == "--help" || cmd == "help") {
    QTextStream(stdout)
        << "Vigil: one shared keep-working switch.\n"
           "vigil open                          Settings and task status\n"
           "vigil status --json                 Inspect state and reason\n"
           "vigil on|off|auto|toggle --json      Manual hold, block tasks, "
           "allow tasks, toggle\n"
           "vigil reserve <5..50> --json         Battery reserve (default "
           "15%)\n"
           "vigil run --name <label> -- <command> [args...]\n"
           "vigil task begin --name <label> --json   Register a 15-minute "
           "lease\n"
           "vigil task renew|finish <token> --json   Renew every 5 minutes or "
           "release\n"
           "vigil sleep --json                  Explicit user-requested sleep\n"
           "vigil cancel --json                 Cancel pending task-completion "
           "sleep; at reserve, return to normal\n"
           "vigil skill install --json          Install concise Codex and "
           "Claude guidance\n"
           "Agent commands: run and task only. Never override a user-disabled "
           "state.\n"
           "All control responses are JSON; failures exit 1 on stderr. Task "
           "tokens are private capabilities.\n";
    return 0;
  }
  if (cmd == "skill" && args.value(0) == "install") {
    QFile resource(":/SKILL.md");
    if (!resource.open(QIODevice::ReadOnly))
      return 1;
    auto body = resource.readAll();
    const QByteArray snippet =
        "\n<!-- vigil:start -->\nFor prolonged local work, use the Vigil skill "
        "(`vigil --help`). Respect user-disabled task requests. For other PC "
        "controls, use `omarchy <group> --help`.\n<!-- vigil:end -->\n";
    for (const QString &agent : {QString(".codex"), QString(".claude")}) {
      QString root = QDir::homePath() + "/" + agent;
      if (!writeFile(root + "/skills/vigil/SKILL.md", body))
        return result(
            {{"ok", false}, {"error", "Cannot install skill in " + root}});
      QString path = root + (agent == ".codex" ? "/AGENTS.md" : "/CLAUDE.md");
      QFile f(path);
      QByteArray old;
      if (f.open(QIODevice::ReadOnly))
        old = f.readAll();
      int begin = old.indexOf("<!-- vigil:start -->"),
          end = old.indexOf("<!-- vigil:end -->");
      if (begin >= 0 && end >= begin)
        old.remove(begin, end + QString("<!-- vigil:end -->").size() - begin);
      if (!writeFile(path, old.trimmed() + "\n" + snippet))
        return result({{"ok", false}, {"error", "Cannot update " + path}});
    }
    return result({{"ok", true}});
  }
  if (cmd == "run") {
    int split = args.indexOf("--");
    QString name;
    int ni = args.indexOf("--name");
    if (ni >= 0)
      name = args.value(ni + 1);
    if (split < 0 || args.size() <= split + 1 || name.isEmpty())
      return result(
          {{"ok", false},
           {"error", "Use vigil run --name <label> -- <command> [args...]"}});
    // Hold for the supervisor's entire lifetime, including child cleanup.
    auto r = callService(
        {{"command", "task-begin"}, {"name", name}, {"pid", double(getpid())}});
    if (!r.value("ok").toBool())
      return result(r);
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    child.setInputChannelMode(QProcess::ForwardedInputChannel);
    child.setChildProcessModifier([] { setsid(); });
    child.start(args[split + 1], args.mid(split + 2));
    if (!child.waitForStarted()) {
      callService({{"command", "task-finish"}, {"id", r.value("id")}});
      return result({{"ok", false}, {"error", child.errorString()}});
    }
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    QElapsedTimer stop;
    while (!child.waitForFinished(250)) {
      if (interrupted) {
        kill(-child.processId(), interrupted);
        interrupted = 0;
        stop.start();
      }
      if (stop.isValid() && stop.elapsed() > 5000)
        kill(-child.processId(), SIGKILL);
    }
    callService({{"command", "task-finish"}, {"id", r.value("id")}});
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 1;
  }
  if (cmd != "open") {
    QJsonObject r{{"command", cmd}};
    if (cmd == "task") {
      r["command"] = "task-" + args.value(0);
      if (args.value(0) == "begin") {
        int n = args.indexOf("--name");
        r["name"] = n >= 0 ? args.value(n + 1) : QString();
      } else
        r["id"] = args.value(1);
    } else if (cmd == "reserve")
      r["value"] = args.value(0).toInt();
    return result(callService(r));
  }
  QQuickStyle::setStyle("Material");
  Translator translator;
  app.installTranslator(&translator);
  Theme theme;
  Backend backend;
  QQmlApplicationEngine engine;
  QObject::connect(&translator, &Translator::changed, &engine,
                   &QQmlEngine::retranslate);
  engine.rootContext()->setContextProperty("theme", &theme);
  engine.rootContext()->setContextProperty("translator", &translator);
  engine.rootContext()->setContextProperty("backend", &backend);
  engine.load(QUrl("qrc:/Main.qml"));
  if (engine.rootObjects().isEmpty())
    return 1;
  if (args.contains("--screenshot")) {
    int n = args.indexOf("--screenshot");
    QString path = args.value(n + 1);
    QTimer::singleShot(1500, &app, [&engine, &app, path] {
      auto w = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
      app.exit(w && w->grabWindow().save(path) ? 0 : 1);
    });
  }
  return app.exec();
}
