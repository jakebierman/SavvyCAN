QT += core testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle

INCLUDEPATH += ..

SOURCES += \
    ai_skill_main.cpp \
    tst_aiactionregistry.cpp \
    ../re/aiactionregistry.cpp

HEADERS += \
    tst_aiactionregistry.h \
    ../re/aiactionregistry.h
