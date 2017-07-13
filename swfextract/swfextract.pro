# SWF extract tool to be built with qmake
# Uses source code from www.github.com/matthiaskramm/swftools

TARGET = swfextract
CONFIG += console

TEMPLATE = app

include(../libs/swflibs_dep.pri)

SOURCES += \
    $$SWFTOOLSROOT/src/swfextract.c \
    jpeg.cpp

HEADERS += \
    $$SWFTOOLSROOT/lib/jpeg.h
