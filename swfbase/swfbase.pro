# Base SWF library project to be built with qmake
# Uses source code from www.github.com/matthiaskramm/swftools

QT -= core gui

TARGET = swfbase
TEMPLATE = lib
CONFIG += staticlib

include(swfbase.pri)

DEFINES += SWFTOOLS_DATADIR=\"\\\".\\\"\"

HEADERS += \
    $$SWFTOOLSROOT/lib/base64.h \
    $$SWFTOOLSROOT/lib/bitio.h \
    $$SWFTOOLSROOT/lib/graphcut.h \
    $$SWFTOOLSROOT/lib/kdtree.h \
    $$SWFTOOLSROOT/lib/log.h \
    $$SWFTOOLSROOT/lib/mem.h \
    $$SWFTOOLSROOT/lib/mp3.h \
    $$SWFTOOLSROOT/lib/os.h \
    $$SWFTOOLSROOT/lib/png.h \
    $$SWFTOOLSROOT/lib/q.h \
    $$SWFTOOLSROOT/lib/ttf.h \
    $$SWFTOOLSROOT/lib/types.h \
    $$SWFTOOLSROOT/lib/utf8.h \
    $$SWFTOOLSROOT/lib/wav.h \
    $$SWFTOOLSROOT/lib/xml.h \
    ../config.h \
    $$SWFTOOLSROOT/lib/args.h

SOURCES += \
    $$SWFTOOLSROOT/lib/base64.c \
    $$SWFTOOLSROOT/lib/bitio.c \
    $$SWFTOOLSROOT/lib/graphcut.c \
    $$SWFTOOLSROOT/lib/kdtree.c \
    $$SWFTOOLSROOT/lib/log.c \
    $$SWFTOOLSROOT/lib/mem.c \
    $$SWFTOOLSROOT/lib/mp3.c \
    $$SWFTOOLSROOT/lib/os.c \
    $$SWFTOOLSROOT/lib/png.c \
    $$SWFTOOLSROOT/lib/q.c \
    $$SWFTOOLSROOT/lib/ttf.c \
    $$SWFTOOLSROOT/lib/utf8.c \
    $$SWFTOOLSROOT/lib/wav.c \
    $$SWFTOOLSROOT/lib/xml.c


