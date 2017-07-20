# Project options to build SWF libraries with qmake
#
# Copyright (c) 2017 Alexandra Cherdantseva

CONFIG += warn_off

CONFIG(debug, debug|release) {
    DEFINES += DEBUG
}

contains(QMAKE_HOST.arch, x86_64) {
    DEFINES += SIZEOF_VOIDP=8
} else {
    DEFINES += SIZEOF_VOIDP=4
}

unix|win32-g++ {
    LIBS += -lz

    QMAKE_CXXFLAGS_WARN_OFF -= -w
    QMAKE_CXXFLAGS += -Wall

    DEFINES += HAVE_UNISTD_H=1
    DEFINES += HAVE_TIME_H=1
    DEFINES += HAVE_SYS_TIME_H=1
    DEFINES += HAVE_SYS_MMAN_H=1
    DEFINES += HAVE_LRAND48=1
    DEFINES += HAVE_DIRENT_H=1
    DEFINES += O_BINARY=0
    DEFINES += boolean=int
} else {
    win32 {
        DEFINES += HAVE_IO_H=1
        DEFINES += strncasecmp=strnicmp
        win32-msvc2013 {
            DEFINES += snprintf=_snprintf
        }

        QMAKE_CXXFLAGS_WARN_OFF -= -W0
        QMAKE_CXXFLAGS += -W3

        INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtZlib
    }
}

isEmpty(QTSWFTOOLSROOT) {
    QTSWFTOOLSROOT = $$_PRO_FILE_PWD_/..
}
isEmpty(SWFTOOLSROOT) {
    SWFTOOLSROOT = $$_PRO_FILE_PWD_/../thirdparty/swftools
}

INCLUDEPATH += $$QTSWFTOOLSROOT/libs
INCLUDEPATH += $$QTSWFTOOLSROOT/libs/dummy
INCLUDEPATH += $$SWFTOOLSROOT/lib
