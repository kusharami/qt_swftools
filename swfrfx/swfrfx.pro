# SWF runtime library project to be built with qmake
# Uses source code from www.github.com/matthiaskramm/swftools

QT -= gui

unix {
    QT -= core
}

TARGET = swfrfx
TEMPLATE = lib
CONFIG += staticlib

include(../swfbase/swfbase.pri)

SOURCES += \
    $$SWFTOOLSROOT/lib/modules/swfaction.c \
    $$SWFTOOLSROOT/lib/modules/swfalignzones.c \
    $$SWFTOOLSROOT/lib/modules/swfbits.c \
    $$SWFTOOLSROOT/lib/modules/swfbutton.c \
    $$SWFTOOLSROOT/lib/modules/swfcgi.c \
    $$SWFTOOLSROOT/lib/modules/swfdraw.c \
    $$SWFTOOLSROOT/lib/modules/swfdump.c \
    $$SWFTOOLSROOT/lib/modules/swffilter.c \
    $$SWFTOOLSROOT/lib/modules/swffont.c \
    $$SWFTOOLSROOT/lib/modules/swfobject.c \
    $$SWFTOOLSROOT/lib/modules/swfrender.c \
    $$SWFTOOLSROOT/lib/modules/swfscripts.c \
    $$SWFTOOLSROOT/lib/modules/swfshape.c \
    $$SWFTOOLSROOT/lib/modules/swfsound.c \
    $$SWFTOOLSROOT/lib/modules/swftext.c \
    $$SWFTOOLSROOT/lib/modules/swftools.c \
    $$SWFTOOLSROOT/lib/rfxswf.c \
    $$SWFTOOLSROOT/lib/drawer.c \
    $$SWFTOOLSROOT/lib/h.263/dct.c \
    $$SWFTOOLSROOT/lib/h.263/h263tables.c \
    $$SWFTOOLSROOT/lib/h.263/swfvideo.c \
    $$SWFTOOLSROOT/lib/action/actioncompiler.c \
    $$SWFTOOLSROOT/lib/action/assembler.c \
    $$SWFTOOLSROOT/lib/action/compile.c \
    $$SWFTOOLSROOT/lib/action/lex.swf4.c \
    $$SWFTOOLSROOT/lib/action/lex.swf5.c \
    $$SWFTOOLSROOT/lib/action/libming.c \
    $$SWFTOOLSROOT/lib/action/swf4compiler.tab.c \
    $$SWFTOOLSROOT/lib/action/swf5compiler.tab.c \
    $$SWFTOOLSROOT/lib/as3/abc.c \
    $$SWFTOOLSROOT/lib/as3/assets.c \
    $$SWFTOOLSROOT/lib/as3/builtin.c \
    $$SWFTOOLSROOT/lib/as3/code.c \
    $$SWFTOOLSROOT/lib/as3/common.c \
    $$SWFTOOLSROOT/lib/as3/compiler.c \
    $$SWFTOOLSROOT/lib/as3/expr.c \
    $$SWFTOOLSROOT/lib/as3/files.c \
    $$SWFTOOLSROOT/lib/as3/import.c \
    $$SWFTOOLSROOT/lib/as3/initcode.c \
    $$SWFTOOLSROOT/lib/as3/opcodes.c \
    $$SWFTOOLSROOT/lib/as3/parser_help.c \
    $$SWFTOOLSROOT/lib/as3/parser.tab.c \
    $$SWFTOOLSROOT/lib/as3/pool.c \
    $$SWFTOOLSROOT/lib/as3/registry.c \
    $$SWFTOOLSROOT/lib/as3/scripts.c \
    $$SWFTOOLSROOT/lib/as3/state.c \
    $$SWFTOOLSROOT/lib/as3/tokenizer.yy.c

HEADERS += \
    $$SWFTOOLSROOT/lib/rfxswf.h \
    $$SWFTOOLSROOT/lib/drawer.h \
    $$SWFTOOLSROOT/lib/h.263/dct.h \
    $$SWFTOOLSROOT/lib/h.263/h263tables.h \
    $$SWFTOOLSROOT/lib/action/action.h \
    $$SWFTOOLSROOT/lib/action/actioncompiler.h \
    $$SWFTOOLSROOT/lib/action/assembler.h \
    $$SWFTOOLSROOT/lib/action/compile.h \
    $$SWFTOOLSROOT/lib/action/libming.h \
    $$SWFTOOLSROOT/lib/action/ming.h \
    $$SWFTOOLSROOT/lib/action/swf4compiler.tab.h \
    $$SWFTOOLSROOT/lib/action/swf5compiler.tab.h \
    $$SWFTOOLSROOT/lib/as3/abc.h \
    $$SWFTOOLSROOT/lib/as3/assets.h \
    $$SWFTOOLSROOT/lib/as3/builtin.h \
    $$SWFTOOLSROOT/lib/as3/code.h \
    $$SWFTOOLSROOT/lib/as3/common.h \
    $$SWFTOOLSROOT/lib/as3/compiler.h \
    $$SWFTOOLSROOT/lib/as3/expr.h \
    $$SWFTOOLSROOT/lib/as3/files.h \
    $$SWFTOOLSROOT/lib/as3/import.h \
    $$SWFTOOLSROOT/lib/as3/initcode.h \
    $$SWFTOOLSROOT/lib/as3/opcodes.h \
    $$SWFTOOLSROOT/lib/as3/parser_help.h \
    $$SWFTOOLSROOT/lib/as3/parser.h \
    $$SWFTOOLSROOT/lib/as3/parser.tab.h \
    $$SWFTOOLSROOT/lib/as3/pool.h \
    $$SWFTOOLSROOT/lib/as3/registry.h \
    $$SWFTOOLSROOT/lib/as3/scripts.h \
    $$SWFTOOLSROOT/lib/as3/state.h \
    $$SWFTOOLSROOT/lib/as3/tokenizer.h


