# Project options to build SWF libraries with qmake
#
# Copyright (c) 2017 Alexandra Cherdantseva

BIN_DIR = $$_PRO_FILE_PWD_/../build/libs

CONFIG(debug, debug|release) {
    BIN_DIR = $$BIN_DIR/Debug
} else {
    BIN_DIR = $$BIN_DIR/Release
}

DESTDIR = $$BIN_DIR

include(../libs/swflibs.pri)

exists($$(QT_SOURCE_PATH)) {
    QT_SOURCE_PATH = $$(QT_SOURCE_PATH)
} else {
    QT_SOURCE_PATH = $$[QT_INSTALL_PREFIX]/../Src
}

QT_THIRDPARTY_PATH = $$QT_SOURCE_PATH/qtbase/src/3rdparty

INCLUDEPATH += $$QT_THIRDPARTY_PATH/freetype/include/

unix|win32-g++ {
    QMAKE_CXXFLAGS += \
        -Wno-unused-function \
        -Wno-unused-variable \
        -Wno-unused-parameter \
        -Wno-bitwise-op-parentheses \
        -Wno-int-to-void-pointer-cast
} else {
    win32 {
        DEFINES += inline=__inline
        DEFINES += YY_NO_UNISTD_H
        DEFINES -= UNICODE _UNICODE
        DEFINES += _MBCS
    }
}
