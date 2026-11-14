QT += core gui qml quick quickcontrols2 dbus network
CONFIG += c++17 release
TARGET = vigil
TEMPLATE = app
HEADERS += src/service.h src/policy.h src/backend.h src/theme.h src/translator.h
SOURCES += src/main.cpp src/service.cpp src/backend.cpp src/theme.cpp src/translator.cpp
RESOURCES += src/resources.qrc
