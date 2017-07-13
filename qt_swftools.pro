# Libraries and tools to decode and encode SWF animation files
#
# Copyright (c) 2017 Alexandra Cherdantseva

TEMPLATE   = subdirs
SUBDIRS   += \
    swfbase \
    swfrfx \
    swfgfx \
    swfgfxreader \
    swfdump \
    swfextract

swfrfx.depends = swfbase
swfgfx.depends = swfrfx
swfgfxreader.depends = swfgfx

swfdump.depends = swfgfxreader
swfextract.depends = swfgfxreader
