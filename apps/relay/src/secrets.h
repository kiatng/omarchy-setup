#pragma once

#include <QObject>
#include <QProcess>
#include <functional>

// API keys in the desktop's secret store (GNOME Keyring, KWallet, or any
// Secret Service), through libsecret's secret-tool. A key never touches a
// file Relay writes, a command line, or the log: it goes to secret-tool on
// stdin and comes back on stdout.
class Secrets : public QObject {
    Q_OBJECT
public:
    explicit Secrets(const QString &program = {}, QObject *parent = nullptr);
    ~Secrets() override;

    bool available() const { return !m_program.isEmpty(); }

    using Found = std::function<void(bool ok, const QString &key)>;
    using Done = std::function<void(bool ok)>;
    void lookup(const QString &provider, Found found);
    void store(const QString &provider, const QString &key, Done done);
    void remove(const QString &provider, Done done);

private:
    void run(const QStringList &arguments, const QByteArray &input,
             std::function<void(bool ok, const QByteArray &out)> done);
    QString m_program;
};
