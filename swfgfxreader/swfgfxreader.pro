# SWF graphics reader library project to be built with qmake
# Uses source code from www.github.com/matthiaskramm/swftools

QT       -= core gui

TARGET = swfgfxreader
TEMPLATE = lib
CONFIG += staticlib

include(../swfbase/swfbase.pri)

HEADERS += \
    $$SWFTOOLSROOT/lib/readers/image.h \
    $$SWFTOOLSROOT/lib/readers/swf.h

SOURCES += \
    $$SWFTOOLSROOT/lib/readers/image.c \
    $$SWFTOOLSROOT/lib/readers/swf.c

