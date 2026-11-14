// Relay — live English captions for the room you're in.

#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "backend.h"
#include "theme.h"
#include "translator.h"

namespace {

const char *Usage =
    "Usage: relay [open|pause|toggle|--help|--version]\n"
    "\n"
    "  open     Show the Relay window, starting it if needed (the default)\n"
    "  pause    Stop listening in a running Relay\n"
    "  toggle   Start or pause listening in a running Relay\n";

QString socketName() {
    return QString("relay-%1").arg(getuid());
}

// Hands a command to the Relay that's already running. True when one was.
bool sendToRunning(const QByteArray &command) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(300))
        return false;
    socket.write(command + "\n");
    socket.waitForBytesWritten(300);
    socket.waitForReadyRead(300);
    return true;
}

int signalPipe[2] = {-1, -1};

void onSignal(int) {
    const char byte = 1;
    [[maybe_unused]] auto written = ::write(signalPipe[1], &byte, 1);
}

} // namespace

int main(int argc, char *argv[]) {
    const QByteArray command = argc > 1 ? QByteArray(argv[1]) : QByteArray("open");
    if (command == "--help" || command == "-h" || command == "help") {
        QTextStream(stdout) << Usage;
        return 0;
    }
    if (command == "--version") {
        QTextStream(stdout) << "relay 0.1.0\n";
        return 0;
    }
    if (command != "open" && command != "pause" && command != "toggle") {
        QTextStream(stderr) << "relay: unknown command " << command << "\n\n" << Usage;
        return 1;
    }

    // A second launch from the bar or launcher raises the first window, so
    // two copies never listen at once.
    if (sendToRunning(command))
        return 0;
    if (command == "pause")
        return 0; // nothing is listening

    QGuiApplication app(argc, argv);
    app.setApplicationName("relay");
    app.setOrganizationName("relay");
    app.setApplicationVersion("0.1.0");

    // Matches the window to relay.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("relay");
    app.setWindowIcon(QIcon::fromTheme("relay"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;

    // Closing by signal (logout, kill) closes the microphone like Quit does.
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe) == 0) {
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [&] {
            char byte;
            [[maybe_unused]] auto got = ::read(signalPipe[0], &byte, 1);
            backend.shutdown();
            app.quit();
        });
        struct sigaction action {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend, &Backend::shutdown);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

    QLocalServer server;
    QLocalServer::removeServer(socketName());
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(socketName());
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                const QByteArray request = socket->readLine().trimmed();
                if (request == "pause") {
                    backend.pause();
                } else if (request == "toggle") {
                    backend.toggle();
                } else if (window) {
                    window->show();
                    window->raise();
                    window->requestActivate();
                    // Wayland only lets a window take focus with a token,
                    // so on Hyprland ask the compositor directly, in the
                    // Lua dispatch form of newer releases and the older one.
                    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
                        QProcess::startDetached("hyprctl", {"dispatch", "hl.dsp.focus({ window = \"class:^(relay)$\" })"});
                        QProcess::startDetached("hyprctl", {"dispatch", "focuswindow", "class:^(relay)$"});
                    }
                }
                socket->write("ok\n");
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    // A toggle that started Relay should start listening once it's up.
    if (command == "toggle")
        QMetaObject::invokeMethod(&backend, &Backend::start, Qt::QueuedConnection);

    return app.exec();
}
