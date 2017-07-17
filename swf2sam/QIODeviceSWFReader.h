// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses libraries from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#pragma once

class QIODevice;

extern "C"
{
struct _reader;
}

class QIODeviceSWFReader
{
public:
	static void init(struct _reader *reader, QIODevice *device);
};
