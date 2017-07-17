// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses libraries from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#include "QIODeviceSWFReader.h"

#include <QIODevice>

#include "bitio.h"

#define READER_TYPE 100

static inline QIODevice *getDevice(reader_t *reader)
{
	Q_ASSERT(nullptr != reader);
	Q_ASSERT(reader->type == READER_TYPE);

	auto device = reinterpret_cast<QIODevice *>(reader->internal);
	Q_ASSERT(nullptr != device);
	return device;
}

extern "C"
{

static int read(reader_t *reader, void *data, int len)
{
	auto device = getDevice(reader);

	return int(device->read(reinterpret_cast<char *>(data), len));

}

static int seek(reader_t *reader, int pos)
{
	auto device = getDevice(reader);

	if (device->seek(pos))
	{
		reader->pos = pos;
		return pos;
	}

	return -1;
}

static void dealloc(reader_t *reader)
{
	auto device = getDevice(reader);

	device->close();

	memset(reader, 0, sizeof(reader_t));
}

}

void QIODeviceSWFReader::init(reader_t *reader, QIODevice *device)
{
	Q_ASSERT(nullptr != reader);
	Q_ASSERT(nullptr != device);

	reader->read = read;
	reader->seek = seek;
	reader->dealloc = dealloc;
	reader->internal = device;
	reader->type = READER_TYPE;
	reader->mybyte = 0;
	reader->bitpos = 8;
	reader->pos = int(device->pos());
}
