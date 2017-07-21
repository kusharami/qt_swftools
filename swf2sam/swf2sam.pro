# SWF to SAM animation converter project file
# Uses Qt Framework from www.qt.io
# Uses libraries from www.github.com/matthiaskramm/swftools

VERSION = 1.0.4

QMAKE_TARGET_PRODUCT = swf2sam
QMAKE_TARGET_DESCRIPTION = SWF to SAM animation converter
QMAKE_TARGET_COPYRIGHT = Copyright (c) 2017 Alexandra Cherdantseva

QT += core gui

CONFIG += c++11

TARGET = swf2sam
CONFIG += console

DEFINES += "APP_VERSION=\"\\\"v$$VERSION\\\"\""
DEFINES += "APP_NAME=\"\\\"$$QMAKE_TARGET_PRODUCT\\\"\""
DEFINES += "APP_DESCRIPTION=\"\\\"$$QMAKE_TARGET_DESCRIPTION\\\"\""

TEMPLATE = app

include(../libs/swflibs_dep.pri)

SOURCES += main.cpp \
    Converter.cpp \
    QIODeviceSWFReader.cpp

HEADERS += \
    Converter.h \
    QIODeviceSWFReader.h

win32 {
    LIBS += -lAdvapi32
    DEFINES += "or=\"||\""
    DEFINES += "and=\"&&\""
    DEFINES += "not=\"!\""
}
