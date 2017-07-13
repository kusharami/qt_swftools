// Part of SWF extract qmake project
// Uses Qt Framework from www.qt.io
// Uses source code from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#include <QImage>
#include <QFile>
#include <QBuffer>

#include "jpeg.h"

extern "C"
{

int jpeg_save(
	unsigned char *data, unsigned int width, unsigned int height,
	int quality, const char *filename)
{
	QImage image(data, int(width), int(height), int(width) * 3,
				 QImage::Format_RGB888);

	if (image.save(QString::fromLocal8Bit(filename), "jpeg", quality))
		return 1;

	return 0;
}

int jpeg_save_gray(
	unsigned char *data, unsigned int width, unsigned int height,
	int quality, const char *filename)
{
	QImage image(data, int(width), int(height), int(width),
				 QImage::Format_Grayscale8);

	if (image.save(QString::fromLocal8Bit(filename), "jpeg", quality))
		return 1;

	return 0;
}

int jpeg_save_to_file(
	unsigned char *data, unsigned int width, unsigned int height,
	int quality, FILE *fi)
{
	QImage image(data, int(width), int(height), int(width) * 3,
				 QImage::Format_RGB888);

	QFile file;

	if (!file.open(fi, QIODevice::WriteOnly | QIODevice::Truncate))
		return 0;

	if (image.save(&file, "jpeg", quality))
		return 1;

	return 0;
}

int jpeg_save_to_mem(
	unsigned char *data, unsigned width, unsigned height, int quality,
	unsigned char *_dest, int _destlen, int components)
{
	QImage::Format format;

	switch (components)
	{
		case 1:
			format = QImage::Format_Grayscale8;
			break;

		case 3:
			format = QImage::Format_RGB888;
			break;

		case 4:
			format = QImage::Format_ARGB32;
			break;

		default:
			// Unsupported components size
			return 0;
	}

	QImage image(data, int(width), int(height),
				 int(width) * components, format);

	auto ba = QByteArray::fromRawData(
			reinterpret_cast<char *>(_dest), _destlen);

	QBuffer buffer(&ba);

	buffer.open(QBuffer::ReadWrite);

	if (image.save(&buffer, "jpeg", quality))
		return int(buffer.size());

	return 0;
}

static void imageLoad(
	QImage image, unsigned char **dest,
	unsigned int *width, unsigned int *height)
{
	Q_ASSERT(nullptr != dest);
	Q_ASSERT(nullptr != width);
	Q_ASSERT(nullptr != height);

	image = image.convertToFormat(QImage::Format_ARGB32);

	int w = image.width();
	int h = image.height();
	int bytesPerLine = w * 4;

	*width = unsigned(w);
	*height = unsigned(h);

	int imageSize = bytesPerLine * h;

	auto destPtr = reinterpret_cast<unsigned char *>(malloc(imageSize));
	*dest = destPtr;

	for (int y = 0; y < h; y++)
	{
		memcpy(destPtr, image.scanLine(y), bytesPerLine);
		destPtr += bytesPerLine;
	}
}

int jpeg_load(
	const char *filename, unsigned char **dest, unsigned int *width,
	unsigned int *height)
{
	QImage image;
	if (not image.load(QString::fromLocal8Bit(filename), "jpeg"))
		return 0;

	imageLoad(image, dest, width, height);
	return 1;
}

int jpeg_load_from_mem(
	unsigned char *_data, int _size, unsigned char **dest,
	unsigned int *width, unsigned int *height)
{
	auto image = QImage::fromData(_data, _size, "jpeg");
	if (image.isNull())
		return 0;

	imageLoad(image, dest, width, height);
	return 1;
}

void jpeg_get_size(
	const char *fname, unsigned int *width, unsigned int *height)
{
	Q_ASSERT(nullptr != width);
	Q_ASSERT(nullptr != height);

	QImage image;
	if (not image.load(QString::fromLocal8Bit(fname), "jpeg"))
	{
		*width = unsigned(image.width());
		*height = unsigned(image.height());
	} else
	{
		*width = 0;
		*height = 0;
	}
}

}
