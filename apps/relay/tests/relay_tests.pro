QT += core gui quick quickcontrols2 multimedia network websockets testlib
CONFIG += c++17 testcase
TARGET = relay_tests
TEMPLATE = app

INCLUDEPATH += ../src
DEFINES += SOURCE_DIR=\\\"$$PWD/../src\\\"

HEADERS += \
    ../src/audio.h ../src/backend.h ../src/captions.h ../src/live.h ../src/microphone.h ../src/pipeline.h \
    ../src/providers.h ../src/secrets.h ../src/segmenter.h ../src/theme.h ../src/translator.h

SOURCES += relay_tests.cpp \
    ../src/audio.cpp ../src/backend.cpp ../src/captions.cpp ../src/live.cpp ../src/microphone.cpp ../src/pipeline.cpp \
    ../src/providers.cpp ../src/secrets.cpp ../src/segmenter.cpp ../src/theme.cpp ../src/translator.cpp
