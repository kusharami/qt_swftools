# SWF graphics library project to be built with qmake
# Uses source code from www.github.com/matthiaskramm/swftools

QT       -= core gui

TARGET = swfgfx
TEMPLATE = lib
CONFIG += staticlib

include(../swfbase/swfbase.pri)

HEADERS += \
    $$SWFTOOLSROOT/lib/gfxdevice.h \
    $$SWFTOOLSROOT/lib/gfxfilter.h \
    $$SWFTOOLSROOT/lib/gfxfont.h \
    $$SWFTOOLSROOT/lib/gfximage.h \
    $$SWFTOOLSROOT/lib/gfxpoly.h \
    $$SWFTOOLSROOT/lib/gfxsource.h \
    $$SWFTOOLSROOT/lib/gfxtools.h \
    $$SWFTOOLSROOT/lib/gfxpoly/active.h \
    $$SWFTOOLSROOT/lib/gfxpoly/convert.h \
    $$SWFTOOLSROOT/lib/gfxpoly/heap.h \
    $$SWFTOOLSROOT/lib/gfxpoly/moments.h \
    $$SWFTOOLSROOT/lib/gfxpoly/poly.h \
    $$SWFTOOLSROOT/lib/gfxpoly/renderpoly.h \
    $$SWFTOOLSROOT/lib/gfxpoly/stroke.h \
    $$SWFTOOLSROOT/lib/gfxpoly/wind.h \
    $$SWFTOOLSROOT/lib/gfxpoly/xrow.h \
    $$SWFTOOLSROOT/lib/devices/bbox.h \
    $$SWFTOOLSROOT/lib/devices/dummy.h \
    $$SWFTOOLSROOT/lib/devices/file.h \
    $$SWFTOOLSROOT/lib/devices/ops.h \
    $$SWFTOOLSROOT/lib/devices/polyops.h \
    $$SWFTOOLSROOT/lib/devices/record.h \
    $$SWFTOOLSROOT/lib/devices/render.h \
    $$SWFTOOLSROOT/lib/devices/rescale.h \
    $$SWFTOOLSROOT/lib/devices/swf.h \
    $$SWFTOOLSROOT/lib/devices/text.h

SOURCES += \
    $$SWFTOOLSROOT/lib/gfxfilter.c \
    $$SWFTOOLSROOT/lib/gfxfont.c \
    $$SWFTOOLSROOT/lib/gfximage.c \
    $$SWFTOOLSROOT/lib/gfxtools.c \
    $$SWFTOOLSROOT/lib/filters/alpha.c \
    $$SWFTOOLSROOT/lib/filters/flatten.c \
    $$SWFTOOLSROOT/lib/filters/one_big_font.c \
    $$SWFTOOLSROOT/lib/filters/remove_font_transforms.c \
    $$SWFTOOLSROOT/lib/filters/remove_invisible_characters.c \
    $$SWFTOOLSROOT/lib/filters/rescale_images.c \
    $$SWFTOOLSROOT/lib/filters/vectors_to_glyphs.c \
    $$SWFTOOLSROOT/lib/gfxpoly/active.c \
    $$SWFTOOLSROOT/lib/gfxpoly/convert.c \
    $$SWFTOOLSROOT/lib/gfxpoly/moments.c \
    $$SWFTOOLSROOT/lib/gfxpoly/poly.c \
    $$SWFTOOLSROOT/lib/gfxpoly/renderpoly.c \
    $$SWFTOOLSROOT/lib/gfxpoly/stroke.c \
    $$SWFTOOLSROOT/lib/gfxpoly/wind.c \
    $$SWFTOOLSROOT/lib/gfxpoly/xrow.c \
    $$SWFTOOLSROOT/lib/devices/bbox.c \
    $$SWFTOOLSROOT/lib/devices/dummy.c \
    $$SWFTOOLSROOT/lib/devices/file.c \
    $$SWFTOOLSROOT/lib/devices/ops.c \
    $$SWFTOOLSROOT/lib/devices/polyops.c \
    $$SWFTOOLSROOT/lib/devices/record.c \
    $$SWFTOOLSROOT/lib/devices/render.c \
    $$SWFTOOLSROOT/lib/devices/rescale.c \
    $$SWFTOOLSROOT/lib/devices/swf.c \
    $$SWFTOOLSROOT/lib/devices/text.c
