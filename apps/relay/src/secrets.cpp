#include "secrets.h"

#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

Secrets::Secrets(const QString &program, QObject *parent)
    : QObject(parent), m_program(!program.isEmpty() ? program : QStandardPaths::findExecutable("secret-tool")) {
    if (!QFileInfo(m_program).isExecutable())
        m_program.clear();
}

// A key being saved as Relay closes still gets saved.
Secrets::~Secrets() {
    for (QProcess *process : findChildren<QProcess *>()) {
        process->disconnect(this);
        if (!process->waitForFinished(3000))
            process->kill();
    }
}

void Secrets::run(const QStringList &arguments, const QByteArray &input,
                  std::function<void(bool, const QByteArray &)> done) {
    if (m_program.isEmpty()) {
        QTimer::singleShot(0, this, [done] { done(false, {}); });
        return;
    }
    auto *process = new QProcess(this);
    // An unlocked keyring answers at once; a locked one shows its own prompt,
    // so give the user time to type the password.
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    auto finished = std::make_shared<bool>(false);
    auto complete = [process, done, finished](bool ok) {
        if (*finished)
            return;
        *finished = true;
        const QByteArray out = ok ? process->readAllStandardOutput().left(4096) : QByteArray();
        process->deleteLater();
        done(ok, out);
    };
    connect(process, &QProcess::finished, this, [complete](int code, QProcess::ExitStatus status) {
        complete(status == QProcess::NormalExit && code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            complete(false);
    });
    process->start(m_program, arguments);
    if (!input.isNull())
        process->write(input);
    process->closeWriteChannel();
    timeout->start(60000);
}

void Secrets::lookup(const QString &provider, Found found) {
    run({"lookup", "application", "relay", "provider", provider}, {}, [found](bool ok, const QByteArray &out) {
        const QString key = QString::fromUtf8(out).trimmed();
        found(ok && !key.isEmpty(), key);
    });
}

void Secrets::store(const QString &provider, const QString &key, Done done) {
    run({"store", "--label", "Relay " + provider + " API key", "application", "relay", "provider", provider},
        key.trimmed().toUtf8(), [done](bool ok, const QByteArray &) { done(ok); });
}

void Secrets::remove(const QString &provider, Done done) {
    run({"clear", "application", "relay", "provider", provider}, {}, [done](bool ok, const QByteArray &) { done(ok); });
}
