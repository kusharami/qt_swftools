# Project options to build SWF libraries with qmake
#
# Copyright (c) 2017 Alexandra Cherdantseva

include(swflibs.pri)

isEmpty(SWFLIBS_PATH) {
    SWFLIBS_PATH = $$QTSWFTOOLSROOT/build/libs
}

CONFIG(debug, debug|release) {
    SWFLIBS_PATH = $$SWFLIBS_PATH/Debug
} else {
    SWFLIBS_PATH = $$SWFLIBS_PATH/Release
}

LIBS += -L$$SWFLIBS_PATH
LIBS += -lswfbase -lswfrfx -lswfgfx -lswfgfxreader
