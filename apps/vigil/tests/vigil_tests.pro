QT += core gui quick quickcontrols2 testlib dbus network
CONFIG += c++17 testcase
TARGET = vigil_tests
TEMPLATE = app
INCLUDEPATH += ../src
HEADERS += ../src/service.h ../src/theme.h ../src/translator.h
SOURCES += vigil_tests.cpp ../src/service.cpp ../src/theme.cpp ../src/translator.cpp

RESOURCES += ../src/resources.qrc
