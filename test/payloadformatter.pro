QT += core testlib serialbus

CONFIG += c++17 console testcase
CONFIG -= app_bundle

TARGET = tst_payloadformatter
TEMPLATE = app

INCLUDEPATH += ..

SOURCES += \
    payloadformatter_main.cpp \
    tst_payloadformatter.cpp \
    ../payloadformatter.cpp

HEADERS += \
    tst_payloadformatter.h \
    ../payloadformatter.h \
    ../payloadformatrouter.h
