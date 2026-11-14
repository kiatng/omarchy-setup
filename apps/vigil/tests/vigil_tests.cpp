#include "policy.h"
#include "service.h"
#include "theme.h"
#include "translator.h"
#include <QJsonArray>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <unistd.h>
class Fake : public Service {
public:
  QTemporaryDir temp;
  bool holding = false, canHold = true;
  int sleeps = 0;
  Fake() {
    runtime = temp.path();
    markerPath = temp.filePath("marker");
    ac = true;
  }
  void hardware() override {}
  bool acquire() override {
    holding = canHold;
    return holding;
  }
  bool held() const override { return holding; }
  void release() override { holding = false; }
  void notify(const QString &) override {}
  void suspend() override {
    sleeps++;
    deadline = 0;
    manual = false;
    allowed = false;
    tasks.clear();
    release();
    setMarker(false);
  }
  void power(bool plug, double percent) {
    ac = plug;
    battery = percent;
  }
  void lidClosed(bool value) { lid = value; }
  void expire() {
    for (auto &t : tasks)
      t.expires = -1;
  }
  void countdownElapsed() { deadline = clock.elapsed() - 1; }
  void markerOn() {
    QFile f(markerPath);
    if (f.open(QIODevice::WriteOnly))
      f.write("external");
  }
  QJsonObject act(QString c) { return request({{"command", c}}); }
  QJsonObject begin() {
    return request({{"command", "task-begin"}, {"name", "Example task"}});
  }
  void advance() {
    QMetaObject::invokeMethod(this, "tick", Qt::DirectConnection);
  }
};
class MockBackend : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap status MEMBER data NOTIFY changed)
  Q_PROPERTY(QString error MEMBER message NOTIFY changed)
public:
  QVariantMap data{
      {"state", "keeping-awake"},
      {"manual", true},
      {"allow_tasks", true},
      {"inhibited", true},
      {"external_power", true},
      {"battery", 83},
      {"reserve", 15},
      {"tasks", QVariantList{QVariantMap{{"name", "Training example model"}}}}};
  QString message;
  int toggles = 0;
  Q_INVOKABLE void action(QString command, int = 0) {
    if (command == "toggle" || command == "off" || command == "on") {
      toggles++;
      data["manual"] = !data["manual"].toBool();
      data["tasks"] = QVariantList{};
      data["inhibited"] = data["manual"];
      data["state"] = data["manual"].toBool() ? "keeping-awake" : "normal";
      emit changed();
    }
  }
signals:
  void changed();
};
class VigilTests : public QObject {
  Q_OBJECT
private slots:
  void qmlInteractionAndLanguage() {
    QQuickStyle::setStyle("Material");
    QTemporaryDir d;
    Theme theme(d.path());
    Translator translator(d.filePath("language"));
    qApp->installTranslator(&translator);
    MockBackend backend;
    QQmlApplicationEngine engine;
    connect(&translator, &Translator::changed, &engine,
            &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(backend.toggles, 1);
    auto control = window->findChild<QQuickItem *>("holdSwitch");
    QVERIFY(control);
    auto point = control->mapToScene(
        QPointF(control->width() / 2, control->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
    QCOMPARE(backend.toggles, 2);
    QVERIFY(control->property("checked").toBool());
    backend.data["tasks"] =
        QVariantList{QVariantMap{{"name", "Training example model"}}};
    emit backend.changed();
    QTest::qWait(300);
    QVERIFY(window->grabWindow().save("../../docs/screenshot.png"));
    QFile language(d.filePath("language"));
    QVERIFY(language.open(QIODevice::WriteOnly));
    language.write("da\n");
    language.close();
    QTRY_COMPARE(translator.language(), QString("da"));
    QTest::qWait(300);
    QVERIFY(window->grabWindow().save("../../docs/screenshot-da.png"));
    qApp->removeTranslator(&translator);
  }
  void reserveRejectsNewTasks() {
    Fake f;
    f.act("auto");
    f.power(false, 15);
    QVERIFY(!f.begin()["ok"].toBool());
  }
  void normalAndAc() {
    QCOMPARE(decide({false, true, false, false, 80, 15}).state,
             QString("normal"));
    QVERIFY(decide({true, true, true, false, 1, 15}).inhibit);
  }
  void batteryBoundaries() {
    QCOMPARE(decide({true, false, true, false, 26, 15}).state,
             QString("keeping-awake"));
    QCOMPARE(decide({true, false, true, false, 25, 15}).state,
             QString("battery-warning"));
    QVERIFY(decide({true, false, true, false, 15, 15}).pending);
    QCOMPARE(decide({true, false, true, false, -1, 15}).state,
             QString("error"));
  }
  void completion() {
    QVERIFY(decide({false, true, true, true, 80, 15}).pending);
    QVERIFY(!decide({false, true, false, true, 80, 15}).inhibit);
  }
  void startupBlocksAgents() {
    Fake f;
    QVERIFY(!f.begin()["ok"].toBool());
  }
  void twoTasks() {
    Fake f;
    f.act("auto");
    auto a = f.begin(), b = f.begin();
    QVERIFY(a["ok"].toBool());
    f.request({{"command", "task-finish"}, {"id", a["id"]}});
    QVERIFY(f.holding);
    QCOMPARE(f.act("status")["tasks"].toArray().size(), 1);
    f.request({{"command", "task-finish"}, {"id", b["id"]}});
    QVERIFY(!f.holding);
  }
  void manualOutlivesTasks() {
    Fake f;
    f.act("on");
    auto a = f.begin();
    f.request({{"command", "task-finish"}, {"id", a["id"]}});
    QVERIFY(f.holding);
    QVERIFY(f.act("status")["manual"].toBool());
  }
  void offRevokesTokens() {
    Fake f;
    f.act("auto");
    auto a = f.begin();
    f.act("off");
    QVERIFY(!f.request({{"command", "task-renew"}, {"id", a["id"]}})["ok"]
                 .toBool());
    QVERIFY(!f.begin()["ok"].toBool());
  }
  void expiredConnectionCountdown() {
    Fake f;
    f.act("auto");
    f.begin();
    f.lidClosed(true);
    f.expire();
    f.advance();
    auto s = f.act("status");
    QCOMPARE(s["state"].toString(), QString("sleep-pending"));
    QCOMPARE(s["last_event"].toString(), QString("task-connection-lost"));
    f.lidClosed(false);
    f.advance();
    QVERIFY(!f.holding);
    QCOMPARE(f.sleeps, 0);
  }
  void powerCancelsReserve() {
    Fake f;
    f.act("on");
    f.power(false, 15);
    f.advance();
    QCOMPARE(f.act("status")["state"].toString(), QString("sleep-pending"));
    f.power(true, 15);
    f.advance();
    QCOMPARE(f.act("status")["countdown"].toInt(), 0);
    QVERIFY(f.holding);
  }
  void batterySuspendsAndResets() {
    Fake f;
    f.act("on");
    f.power(false, 15);
    f.advance();
    f.countdownElapsed();
    f.advance();
    QCOMPARE(f.sleeps, 1);
    QVERIFY(!f.begin()["ok"].toBool());
  }
  void explicitOnConvertsTaskHoldToManual() {
    Fake f;
    f.act("auto");
    auto task = f.begin();
    QTest::qWait(5);
    f.markerOn();
    f.advance();
    f.request({{"command", "task-finish"}, {"id", task["id"]}});
    QVERIFY(f.act("status")["manual"].toBool());
    QVERIFY(f.holding);
  }
  void markerUsesSameState() {
    Fake f;
    f.markerOn();
    f.advance();
    QVERIFY(f.act("status")["manual"].toBool());
    QVERIFY(f.holding);
  }
  void failureNotReportedAsProtected() {
    Fake f;
    f.canHold = false;
    f.act("auto");
    QVERIFY(!f.begin()["ok"].toBool());
    QVERIFY(!f.act("status")["inhibited"].toBool());
  }
  void processIdentity() {
    QVERIFY(!processStart(getpid()).isEmpty());
    QVERIFY(processStart(999999999).isEmpty());
  }
  void themeFallback() {
    QTemporaryDir d;
    Theme t(d.path());
    QCOMPARE(t.accent(), QString("#FFD60A"));
  }
  void translationFallback() {
    QTemporaryDir d;
    Translator t(d.filePath("lang"), d.path());
    QCOMPARE(t.language(), QString("en"));
  }
};
QTEST_MAIN(VigilTests)
#include "vigil_tests.moc"
