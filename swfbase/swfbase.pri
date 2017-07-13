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

QT_THIRDPARTY_PATH = $$[QT_INSTALL_PREFIX]/../Src/qtbase/src/3rdparty

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
        INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtZlib
    }
}
