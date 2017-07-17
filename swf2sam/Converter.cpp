// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses libraries from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#include "Converter.h"

#include "QIODeviceSWFReader.h"

#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QtMath>
#include <QImage>
#include <QSaveFile>
#include <QBuffer>
#include <QDebug>

#include <zlib.h>

#include "rfxswf.h"

#define SAM_VERSION 1
#define TWIPS_PER_PIXEL (20)
#define TWIPS_PER_PIXELF (20.0)
#define LONG_TO_FLOAT (65536.0)
#define WORD_TO_FLOAT (256.0)

#define FRAMEFLAGS_REMOVES 0x01
#define FRAMEFLAGS_ADDS 0x02
#define FRAMEFLAGS_MOVES 0x04
#define FRAMEFLAGS_FRAME_NAME 0x08

#define MOVEFLAGS_ROTATE 0x4000
#define MOVEFLAGS_COLOR 0x2000
#define MOVEFLAGS_MATRIX 0x1000
#define MOVEFLAGS_LONGCOORDS 0x0800

#define DEPTH_MASK 0x3FF
#define DEPTH_MAX DEPTH_MASK

#define SAM_SIGN_SIZE 4
#define SAM_VERSION 1

static const char SAM_Signature[] = "MAS.";

struct SAM_Header
{
	char signature[SAM_SIGN_SIZE];
	quint32 version;
	quint8 frame_rate;
	qint32 x;
	qint32 y;
	qint32 width;
	qint32 height;
};

Converter::Converter()
	: mScale(1.0)
	, mResult(OK)
{

}

void Converter::loadConfig(const QString &configFilePath)
{
	if (configFilePath.isEmpty())
	{
		mLabelRenameMap.clear();
		return;
	}

	QFile file(configFilePath);

	if (!file.open(QFile::ReadOnly))
	{
		mResult = CONFIG_OPEN_ERROR;
		return;
	}

	loadConfigJson(file.readAll());
}

void Converter::loadConfigJson(const QByteArray &json)
{
	QJsonParseError error;
	auto doc = QJsonDocument::fromJson(json, &error);

	if (error.error != QJsonParseError::NoError or not doc.isObject())
	{
		mResult = CONFIG_PARSE_ERROR;
		return;
	}

	auto obj = doc.object();

	auto rename = obj.value(QLatin1String("rename_labels"));

	if (not rename.isUndefined() and not rename.isObject())
	{
		mResult = CONFIG_PARSE_ERROR;
		return;
	}

	if (rename.isUndefined())
		return;

	auto renameObj = rename.toObject();
	LabelRenameMap renameMap;

	for (auto it = renameObj.constBegin(); it != renameObj.constEnd(); ++it)
	{
		auto value = it.value();

		if (value.isString())
		{
			renameMap[value.toString()] = it.key();
			continue;
		}
		if (value.isArray())
		{
			auto arr = value.toArray();

			bool ok = true;
			for (auto ait = arr.constBegin(); ait != arr.constEnd(); ++ait)
			{
				auto av = *ait;

				if (!av.isString())
				{
					ok = false;
					break;
				}

				renameMap[av.toString()] = it.key();
			}

			if (ok)
				continue;
		}
		mResult = CONFIG_PARSE_ERROR;
		return;
	}

	renameMap.swap(mLabelRenameMap);
}

struct Image
{
	TAG *tag;
	TAG *jpegTables;
	size_t index;

	QVariant errorInfo;

	QString fileName;

	Image(TAG *tag, TAG *jpegTables, size_t index);

	int id() const;

	int exportImage(const QString &prefix, qreal scale);
};

struct Shape
{
	size_t index;
	size_t imageIndex;

	int width;
	int height;
	MATRIX matrix;

	Shape(size_t index);
};

struct Frame
{
	struct ObjectAdd
	{
		quint16 depth;
		quint8 shapeId;
	};

	struct ObjectMove
	{
		quint16 depthAndFlags;
		MATRIX matrix;
		RGBA color;
	};

	using Removes = std::vector<quint16>;
	using Adds = std::vector<ObjectAdd>;
	using Moves = std::vector<ObjectMove>;

	QString labelName;

	Removes removes;
	Adds adds;
	Moves moves;
};

static inline quint8 cxToByte(S16 cx)
{
	return quint8((cx / WORD_TO_FLOAT) * 255.0);
}

struct Converter::Process
{
	SWF swf;
	std::vector<Image> images;
	std::vector<Shape> shapes;
	std::vector<Frame> frames;

	std::map<int, size_t> imageMap;
	std::map<int, size_t> shapeMap;

	LabelRenameMap renames;

	QString prefix;
	QVariant errorInfo;

	Converter *owner;
	Frame *currentFrame;
	TAG *jpegTables;
	int result;

	Process(Converter *owner);
	~Process();

	inline int scale(int value, int mode) const;

	bool handleShowFrame();
	bool handleFrameLabel(TAG *tag);
	bool handlePlaceObject(TAG *tag);
	bool handleRemoveObject(TAG *tag);
	bool handleImage(TAG *tag);
	bool handleShape(TAG *tag);
	bool readSWF();
	bool parseSWF();
	bool exportSAM();

	bool writeSAMHeader(QDataStream &stream);
	bool writeSAMShapes(QDataStream &stream);
	bool writeSAMFrames(QDataStream &stream);
	bool writeSAMString(QDataStream &stream, const QString &str);

	bool writeSAMFrameRemoves(
		QDataStream &stream, const Frame::Removes &removes);
	bool writeSAMFrameAdds(QDataStream &stream, const Frame::Adds &adds);
	bool writeSAMFrameMoves(QDataStream &stream, const Frame::Moves &moves);
	bool writeSAMFrameLabel(QDataStream &stream, const QString &labelName);

	bool outputStreamOk(QDataStream &stream);
};

int Converter::exec()
{
	Process process(this);
	mResult = process.result;
	mErrorInfo = process.errorInfo;
	return mResult;
}

QString Converter::errorMessage() const
{
	switch (mResult)
	{
		case INPUT_FILE_OPEN_ERROR:
			return "Unable to open SWF file.";

		case INPUT_FILE_FORMAT_ERROR:
			return "SWF file format error.";

		case INPUT_FILE_BAD_DATA_ERROR:
			return QString("Broken SWF file (%1).")
				   .arg(mErrorInfo.toString());

		case UNSUPPORTED_LINESTYLES:
			return "Cannot export line styles to SAM.";

		case UNSUPPORTED_FILLSTYLE:
			return QString("Cannot export fill style 0x%1 to SAM.")
				   .arg(mErrorInfo.toInt(), 2, 16, QChar('0'));

		case UNSUPPORTED_SHAPE:
			return QString("Cannot export shape 0x%1 to SAM.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_OBJECT_FLAGS:
			return QString("Cannot export object with flags 0x%1 to SAM.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_OBJECT_DEPTH:
			return QString("Cannot export object with depth 0x%1 to SAM.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_SHAPE_COUNT:
			return QString(
				"Cannot export more than 255 shapes to SAM.")
				   .arg(mErrorInfo.toInt());

		case UNSUPPORTED_DISPLAY_COUNT:
			return "Cannot export more than 255 places and/or removes to SAM.";

		case UNSUPPORTED_ADD_COLOR:
			return QString(
				"Cannot export additional color for object 0x%1 to SAM.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_TAG:
		{
			TAG tag;
			tag.id = quint16(mErrorInfo.toInt());
			QString tagName(swf_TagGetName(&tag));
			if (tagName.isEmpty())
				tagName = QString::number(tag.id);
			return QString("Cannot export tag '%1' to SAM.")
				   .arg(tagName);
		}

		case UNKNOWN_IMAGE_ID:
			return QString("Unknown image id 0x%1.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case UNKNOWN_SHAPE_ID:
			return QString("Unknown shape id 0x%1.")
				   .arg(mErrorInfo.toInt(), 4, 16, QChar('0'));

		case OUTPUT_DIR_ERROR:
			return "Unable to make output directory.";

		case OUTPUT_FILE_WRITE_ERROR:
			return QString("Unable to write file '%1'.")
				   .arg(QFileInfo(mErrorInfo.toString()).fileName());

		case CONFIG_OPEN_ERROR:
			return "Unable to open configuration file.";

		case CONFIG_PARSE_ERROR:
			return "Unable to parse configuration file.";

		case BAD_SCALE_VALUE:
			return "Bad scale value.";
	}

	return QString();
}

QString Converter::outputFilePath(const QString &fileName) const
{
	QDir outDir(mOutputDirPath);

	return outDir.filePath(fileName);
}

int Converter::scale(int value, int mode) const
{
	qreal v = value * mScale;

	switch (mode)
	{
		case FLOOR:
			return qFloor(v);

		case CEIL:
			return qCeil(v);

		default:
			break;
	}

	return int(v);
}

Image::Image(TAG *tag, TAG *jpegTables, size_t index)
	: tag(tag)
	, jpegTables(jpegTables)
	, index(index)
{
	Q_ASSERT(nullptr != tag);
}

int Image::id() const
{
	return GET16(tag->data);
}

static int findjpegboundary(U8 *data, int len)
{
	int t;
	int pos = -1;
	for (t = 0; t < len - 4; t++)
	{
		if (data[t] == 0xff &&
			data[t + 1] == 0xd9 &&
			data[t + 2] == 0xff &&
			data[t + 3] == 0xd8)
		{
			pos = t;
		}
	}
	return pos;
}

static QByteArray tagInflate(TAG *t, int len)
{
	QByteArray result(len, Qt::Uninitialized);

	auto destlen = uLongf(len);
	if (Z_OK != uncompress(
			reinterpret_cast<Bytef *>(result.data()), &destlen,
			&t->data[t->pos], t->len - t->pos))
	{
		return QByteArray();
	}

	result.resize(int(destlen));
	return result;
}

int Image::exportImage(const QString &prefix, qreal scale)
{
	static const QString nameFmt("%1_%2.png");

	auto imageFilePath = nameFmt.arg(prefix).arg(index + 1, 4, 10, QChar('0'));

	fileName = QFileInfo(imageFilePath).fileName();

	int writeLen;
	int tagEnd = tag->len;

	QImage image;

	switch (tag->id)
	{
		default:
			Q_UNREACHABLE();
			break;

		case ST_DEFINEBITSJPEG:
		case ST_DEFINEBITSJPEG2:
		{
			QBuffer jpegBuffer;

			jpegBuffer.open(QBuffer::WriteOnly);

			int skip = 2;
			switch (tag->id)
			{
				case ST_DEFINEBITSJPEG:
				{
					if (nullptr != jpegTables && jpegTables->len >= 2)
					{
						writeLen = jpegTables->len - 2;
						skip += 2;
						if (jpegBuffer.write(
								reinterpret_cast<char *>(jpegTables->data),
								writeLen) != writeLen)
						{
							return Converter::OUTPUT_FILE_WRITE_ERROR;
						}
					}
					break;
				}

				case ST_DEFINEBITSJPEG2:
				{
					int pos = findjpegboundary(&tag->data[2], tagEnd - 2);

					if (pos >= 0)
					{
						writeLen = pos;

						if (jpegBuffer.write(
								reinterpret_cast<char *>(&tag->data[2]),
								writeLen) != writeLen)
						{
							return Converter::OUTPUT_FILE_WRITE_ERROR;
						}

						skip += pos + 4;
					}
					break;
				}
			}

			writeLen = tagEnd - skip;
			if (writeLen > 0 && jpegBuffer.write(
					reinterpret_cast<char *>(&tag->data[skip]),
					writeLen) != writeLen)
			{
				return Converter::OUTPUT_FILE_WRITE_ERROR;
			}

			jpegBuffer.close();

			auto &bytes = jpegBuffer.buffer();

			image = QImage::fromData(
					reinterpret_cast<const quint8 *>(
						bytes.constData()), bytes.count());

			if (image.isNull())
			{
				errorInfo = QString("Jpeg load failed");
				return Converter::INPUT_FILE_BAD_DATA_ERROR;
			}

			break;
		}

		case ST_DEFINEBITSJPEG3:
		{
			if (tagEnd > 6)
			{
				int end = GET32(&tag->data[2]);

				image = QImage::fromData(&tag->data[6], end);

				if (image.isNull())
				{
					errorInfo = QString("Jpeg load failed");
					return Converter::INPUT_FILE_BAD_DATA_ERROR;
				}

				end += 6;

				int compressedAlphaSize = tagEnd - end;
				if (compressedAlphaSize > 0 && not image.hasAlphaChannel())
				{
					int width = image.width();
					int height = image.height();

					int alphaSize = width * height;
					auto uncompressedSize = uLongf(alphaSize);
					std::unique_ptr<Bytef[]> data(new Bytef[uncompressedSize]);
					if (Z_OK != uncompress(
							data.get(), &uncompressedSize,
							&tag->data[end],
							uLong(compressedAlphaSize)))
					{
						errorInfo = QString("Jpeg alpha failed");
						return Converter::INPUT_FILE_BAD_DATA_ERROR;
					}

					if (uncompressedSize != uLong(alphaSize))
					{
						errorInfo = QString("Jpeg alpha failed");
						return Converter::INPUT_FILE_BAD_DATA_ERROR;
					}

					image = image.convertToFormat(QImage::Format_RGBA8888);

					auto srcAlpha = data.get();

					for (int y = 0; y < height; y++)
					{
						auto dstAlpha = image.scanLine(y) + 3;

						for (int x = 0; x < width; x++)
						{
							*dstAlpha = *srcAlpha++;
							dstAlpha += 4;
						}
					}
				}
			}

			break;
		}

		case ST_DEFINEBITSLOSSLESS:
		case ST_DEFINEBITSLOSSLESS2:
		{
			swf_GetU16(tag);	// skip index
			int bpp = 1 << swf_GetU8(tag);

			bool alpha = (tag->id == ST_DEFINEBITSLOSSLESS2);

			int width = swf_GetU16(tag);
			int height = swf_GetU16(tag);

			int colorTableSize = 0;
			switch (bpp)
			{
				case 8:
				{
					colorTableSize = swf_GetU8(tag) + 1;
					image = QImage(width, height, QImage::Format_Indexed8);
					break;
				}

				case 16:
				{
					image = QImage(width, height, QImage::Format_RGB555);
					break;
				}

				case 32:
				{
					image = QImage(
							width, height, alpha
							? QImage::Format_RGBA8888_Premultiplied
							: QImage::Format_RGBX8888);

					break;
				}

				default:
					errorInfo = QString("Bad bits per pixel.");
					return Converter::INPUT_FILE_BAD_DATA_ERROR;
			}

			int widthBytes = width * (bpp / 8);
			int bytesPerLine = (widthBytes + 3) & ~3;
			int imageSize = bytesPerLine * height;

			auto data = tagInflate(
					tag, imageSize + colorTableSize * (3 + (alpha ? 1 : 0)));

			auto src = reinterpret_cast<const U8 *>(data.constData());

			if (colorTableSize > 0)
			{
				QVector<QRgb> colorTable(colorTableSize);

				for (int i = 0; i < colorTableSize; i++)
				{
					int r = *src++;
					int g = *src++;
					int b = *src++;
					int a = alpha ? *src++ : 255;

					colorTable[i] = qRgba(r, g, b, a);
				}

				image.setColorTable(colorTable);
			}

			switch (bpp)
			{
				case 8:
				case 16:
					for (int y = 0; y < height; y++)
					{
						memcpy(image.scanLine(y), src, widthBytes);
						src += bytesPerLine;
					}
					break;

				case 32:
				{
					int srcPadding = bytesPerLine - widthBytes;
					for (int y = 0; y < height; y++)
					{
						auto destLine = image.scanLine(y);
						for (int x = 0; x < width; x++)
						{
							quint8 a = *src++;
							quint8 r = *src++;
							quint8 g = *src++;
							quint8 b = *src++;
							*destLine++ = r;
							*destLine++ = g;
							*destLine++ = b;
							*destLine++ = alpha ? a : 255;
						}

						src += srcPadding;
					}
					break;
				}

				default:
					Q_UNREACHABLE();
					break;
			}

			break;
		}
	}

	Q_ASSERT(not image.isNull());

	int scaledWidth = qCeil(image.width() * scale);
	int scaledHeight = qCeil(image.height() * scale);

	if (scaledWidth <= 0 || scaledWidth > 16386 ||
		scaledHeight <= 0 || scaledHeight > 16386)
	{
		return Converter::BAD_SCALE_VALUE;
	}

	if (scaledWidth != image.width() || scaledHeight != image.height())
	{
		image = image.scaled(
				scaledWidth, scaledHeight,
				Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	}

	if (not QDir().mkpath(QFileInfo(prefix).path()))
	{
		return Converter::OUTPUT_DIR_ERROR;
	}

	QSaveFile file(imageFilePath);

	if (not file.open(QFile::WriteOnly | QFile::Truncate))
	{
		return Converter::OUTPUT_FILE_WRITE_ERROR;
	}

	if (not image.save(&file, "png") || not file.commit())
	{
		return Converter::OUTPUT_FILE_WRITE_ERROR;
	}

	qInfo().noquote() << fileName;
	return Converter::OK;
}

Shape::Shape(size_t index)
	: index(index)
	, imageIndex(0xFFFFFFFF)
	, width(0)
	, height(0)
{
	memset(&matrix, 0, sizeof(MATRIX));
	matrix.sx = 65536 * TWIPS_PER_PIXEL;
	matrix.sy = 65536 * TWIPS_PER_PIXEL;
}

bool Converter::Process::handleShowFrame()
{
	if (currentFrame == nullptr)
	{
		errorInfo = QString("Show frame failed");
		result = INPUT_FILE_BAD_DATA_ERROR;
		return false;
	}

	currentFrame++;
	if (currentFrame > &frames.back())
	{
		currentFrame = nullptr;
	}

	return true;
}

bool Converter::Process::handleFrameLabel(TAG *tag)
{
	if (currentFrame == nullptr)
	{
		errorInfo = QString("Frame label failed");
		result = INPUT_FILE_BAD_DATA_ERROR;
		return false;
	}

	auto str = reinterpret_cast<char *>(tag->data);
	auto labelName =
		swf.fileVersion >= 6
		? QString::fromUtf8(str)
		: QString::fromLocal8Bit(str);

	auto &renameMap = owner->mLabelRenameMap;
	auto it = renameMap.find(labelName);
	if (it != renameMap.end())
	{
		currentFrame->labelName = it->second;
	} else
	{
		currentFrame->labelName = labelName;
	}

	renames[labelName] = currentFrame->labelName;

	return true;
}

bool Converter::Process::handlePlaceObject(TAG *tag)
{
	if (currentFrame == nullptr)
	{
		errorInfo = QString("Place object failed");
		result = INPUT_FILE_BAD_DATA_ERROR;
		return false;
	}

	SWFPLACEOBJECT srcObj;
	swf_GetPlaceObject(tag, &srcObj);

	if (srcObj.flags & ~(PF_CHAR | PF_CXFORM |
						 PF_MATRIX | PF_MOVE | PF_NAME))
	{
		errorInfo = srcObj.flags;
		result = UNSUPPORTED_OBJECT_FLAGS;
		return false;
	}

	if (srcObj.depth > DEPTH_MAX)
	{
		errorInfo = srcObj.depth;
		result = UNSUPPORTED_OBJECT_DEPTH;
		return false;
	}

	quint16 depth = srcObj.depth;

	bool placeObject1 = (tag->id == ST_PLACEOBJECT);

	bool shouldMove = (placeObject1 ||
					   0 != (srcObj.flags & PF_MOVE));

	if (placeObject1 || 0 != (srcObj.flags & PF_CHAR))
	{
		if (shouldMove)
		{
			if (placeObject1)
			{
				errorInfo = srcObj.flags;
				result = UNSUPPORTED_OBJECT_FLAGS;
				return false;
			}

			auto &removes = currentFrame->removes;

			if (removes.size() == 255)
			{
				result = UNSUPPORTED_DISPLAY_COUNT;
				return false;
			}

			removes.push_back(depth);
		}

		auto shapeIt = shapeMap.find(srcObj.id);
		if (shapeIt == shapeMap.end() || shapeIt->second > 255)
		{
			errorInfo = srcObj.id;
			result = UNKNOWN_SHAPE_ID;
			return false;
		}

		Frame::ObjectAdd add;
		add.depth = depth;
		add.shapeId = quint8(shapeIt->second);

		auto &adds = currentFrame->adds;
		if (adds.size() == 255)
		{

			result = UNSUPPORTED_DISPLAY_COUNT;
			return false;
		}

		adds.push_back(add);
	}

	Frame::ObjectMove move;
	move.depthAndFlags = 0;

	if (placeObject1 || 0 != (srcObj.flags & PF_CXFORM))
	{
		if (srcObj.cxform.a1 != 0 ||
			srcObj.cxform.r1 != 0 ||
			srcObj.cxform.g1 != 0 ||
			srcObj.cxform.b1 != 0)
		{
			errorInfo = srcObj.id;
			result = UNSUPPORTED_ADD_COLOR;
			return false;
		}
		move.depthAndFlags |= MOVEFLAGS_COLOR;
	}

	if (placeObject1 || 0 != (srcObj.flags & PF_MATRIX))
	{
		move.depthAndFlags |= MOVEFLAGS_MATRIX;
	}

	if (move.depthAndFlags != 0)
	{
		move.depthAndFlags |= depth;
		move.matrix = srcObj.matrix;

		move.color.r = cxToByte(srcObj.cxform.r0);
		move.color.g = cxToByte(srcObj.cxform.g0);
		move.color.b = cxToByte(srcObj.cxform.b0);
		move.color.a = cxToByte(srcObj.cxform.a0);

		auto &moves = currentFrame->moves;
		if (moves.size() == 255)
		{
			result = UNSUPPORTED_DISPLAY_COUNT;
			return false;
		}

		moves.push_back(move);
	}

	return true;
}

bool Converter::Process::handleRemoveObject(TAG *tag)
{
	if (currentFrame == nullptr)
	{
		errorInfo = QString("Remove object failed");
		result = INPUT_FILE_BAD_DATA_ERROR;
		return false;
	}

	auto &removes = currentFrame->removes;

	if (removes.size() == 255)
	{
		result = UNSUPPORTED_DISPLAY_COUNT;
		return false;
	}

	removes.push_back(quint16(swf_GetDepth(tag)));

	return true;
}

bool Converter::Process::handleImage(TAG *tag)
{
	auto index = images.size();
	images.push_back(Image(tag, jpegTables, index));
	imageMap[GET16(tag->data)] = index;

	Image &image = images.back();
	result = image.exportImage(prefix, owner->mScale);

	switch (result)
	{
		case OK:
			break;

		case BAD_SCALE_VALUE:
		case INPUT_FILE_BAD_DATA_ERROR:
		case OUTPUT_DIR_ERROR:
			errorInfo = image.errorInfo;
			return false;

		case OUTPUT_FILE_WRITE_ERROR:
			errorInfo = QDir(owner->mOutputDirPath).filePath(image.fileName);
			return false;

		default:
			Q_UNREACHABLE();
			return false;
	}

	return true;
}

bool Converter::Process::handleShape(TAG *tag)
{
	SHAPE2 srcShape;

	swf_ParseDefineShape(tag, &srcShape);
	int shapeId = GET16(tag->data);
	errorInfo = shapeId;

	if (srcShape.numlinestyles > 0)
	{
		result = UNSUPPORTED_LINESTYLES;
		return false;
	}

	Image *img = nullptr;

	auto index = shapes.size();

	if (index == 255)
	{
		result = UNSUPPORTED_SHAPE_COUNT;
		return false;
	}

	shapes.push_back(Shape(index));
	shapeMap[shapeId] = index;
	Shape &shape = shapes.back();

	for (int i = 0; i < srcShape.numfillstyles; i++)
	{
		auto &fillStyle = srcShape.fillstyles[i];

		switch (fillStyle.type)
		{
			case 0x40:
			case 0x41:
			case 0x42:
			case 0x43:
			{
				int imageId = fillStyle.id_bitmap;
				if (imageId == 65535)
					continue;

				auto it = imageMap.find(imageId);
				if (it == imageMap.end())
				{
					errorInfo = imageId;
					result = UNKNOWN_IMAGE_ID;
					return false;
				}

				if (nullptr != img)
				{
					result = UNSUPPORTED_SHAPE;
					return false;
				}

				shape.imageIndex = it->second;
				img = &images.at(shape.imageIndex);
				Q_ASSERT(nullptr != img);

				shape.matrix = fillStyle.m;
				break;
			}

			default:
				errorInfo = fillStyle.type;
				result = UNSUPPORTED_FILLSTYLE;
				return false;
		}
	}

	if (nullptr == img)
	{
		result = UNSUPPORTED_SHAPE;
		return false;
	}

	auto line = srcShape.lines;

	int minX = 0;
	int minY = 0;
	int maxX = 0;
	int maxY = 0;
	int curX = 0, curY = 0;
	int lineCount = 0;
	bool ok = true;
	while (line && ok)
	{
		switch (line->type)
		{
			case lineTo:
				if (line != srcShape.lines)
				{
					if (line->next == nullptr)
					{
						if (lineCount == 4 &&
							line->y == srcShape.lines->y &&
							line->x == srcShape.lines->x)
						{
							// total lines valid 4
							// endpoint valid
							break;
						}
					} else
					{
						if (line->y == curY && line->x != curX)
						{
							curX = line->x;
							if (curX < minX)
								minX = curX;

							if (curX > maxX)
								maxX = curX;

							// horizontal line valid
							break;
						}

						if (line->y != curY && line->x == curX)
						{
							curY = line->y;
							if (curY < minY)
								minY = curY;

							if (curY > maxY)
								maxY = curY;

							// vertical line valid
							break;
						}
					}
				}

				ok = false;
				break;

			case moveTo:
			{
				if (line == srcShape.lines)
				{
					curX = line->x;
					curY = line->y;
					minX = curX;
					minY = curY;
					maxX = curX;
					maxY = curY;
					break;
				}
				// fall through
			}

			default:
				ok = false;
				break;
		}

		if (ok)
		{
			lineCount++;
			line = line->next;
		} else
		{
			result = UNSUPPORTED_SHAPE;
		}
	}

	if (ok)
	{
		int testX, testY;
		if (shape.matrix.sx < 0)
			testX = maxX;
		else
			testX = minX;

		if (shape.matrix.sy < 0)
			testY = maxY;
		else
			testY = minY;

		if (testX == shape.matrix.tx && testY == shape.matrix.ty)
		{
			shape.width = maxX - minX;
			shape.height = maxY - minY;
			return true;
		}

		result = UNSUPPORTED_SHAPE;
	}

	return false;
}

bool Converter::Process::readSWF()
{
	QFile inputFile(owner->mInputFilePath);

	if (not inputFile.open(QFile::ReadOnly))
	{
		result = INPUT_FILE_OPEN_ERROR;
		return false;
	}

	if (not QDir().mkpath(owner->mOutputDirPath))
	{
		result = OUTPUT_DIR_ERROR;
		return false;
	}

	reader_t reader;
	QIODeviceSWFReader::init(&reader, &inputFile);

	bool ok = swf_ReadSWF2(&reader, &swf) >= 0;

	reader.dealloc(&reader);

	if (not ok)
	{
		result = INPUT_FILE_FORMAT_ERROR;
	}

	return ok;
}

bool Converter::Process::parseSWF()
{
	frames.resize(swf.frameCount);

	currentFrame = &frames.front();

	auto tag = swf.firstTag;
	bool ok = true;
	while (tag && ok)
	{
		switch (tag->id)
		{
			case ST_FILEATTRIBUTES:
			case ST_SETBACKGROUNDCOLOR:
			case ST_SCENEDESCRIPTION:
			case ST_METADATA:
			case ST_END:
				// ignore
				break;

			case ST_SHOWFRAME:
				ok = handleShowFrame();
				break;

			case ST_FRAMELABEL:
				ok = handleFrameLabel(tag);
				break;

			case ST_PLACEOBJECT:
			case ST_PLACEOBJECT2:
			case ST_PLACEOBJECT3:
				ok = handlePlaceObject(tag);
				break;

			case ST_REMOVEOBJECT:
			case ST_REMOVEOBJECT2:
				ok = handleRemoveObject(tag);
				break;

			case ST_JPEGTABLES:
				jpegTables = tag;
				break;

			case ST_DEFINEBITSLOSSLESS:
			case ST_DEFINEBITSLOSSLESS2:
			case ST_DEFINEBITSJPEG:
			case ST_DEFINEBITSJPEG2:
			case ST_DEFINEBITSJPEG3:
				ok = handleImage(tag);
				break;

			case ST_DEFINESHAPE:
			case ST_DEFINESHAPE2:
			case ST_DEFINESHAPE3:
			case ST_DEFINESHAPE4:
				ok = handleShape(tag);
				break;

			default:
				ok = false;
				errorInfo = tag->id;
				result = UNSUPPORTED_TAG;
				break;
		}

		tag = swf_NextTag(tag);
	}

	return ok;
}

bool Converter::Process::exportSAM()
{
	QSaveFile samFile(prefix + ".sam");

	errorInfo = samFile.fileName();
	if (not samFile.open(QFile::WriteOnly | QFile::Truncate))
	{
		result = OUTPUT_FILE_WRITE_ERROR;
		return false;
	}

	{
		QDataStream stream(&samFile);
		stream.setByteOrder(QDataStream::LittleEndian);

		if (not writeSAMHeader(stream) ||
			not writeSAMShapes(stream) ||
			not writeSAMFrames(stream))
		{
			return false;
		}
	}	// close data stream

	if (not samFile.commit())
	{
		result = OUTPUT_FILE_WRITE_ERROR;
		return false;
	}

	qInfo().noquote() << QFileInfo(samFile.fileName()).fileName();
	qInfo().noquote() << QString("Labels:");
	for (auto &it : renames)
	{
		if (it.first != it.second)
		{
			qInfo().noquote() << QString("%1 -> %2").arg(it.first, it.second);
		} else
		{
			qInfo().noquote() << it.first;
		}
	}
	return true;
}

bool Converter::Process::writeSAMHeader(QDataStream &stream)
{
	SAM_Header header;
	memcpy(header.signature, SAM_Signature, SAM_SIGN_SIZE);
	header.version = SAM_VERSION;
	header.frame_rate = quint8(swf.frameRate >> 8);
	header.x = scale(swf.movieSize.xmin, FLOOR);
	header.y = scale(swf.movieSize.ymin, FLOOR);
	header.width = scale(swf.movieSize.xmax, CEIL) - header.x;
	header.height = scale(swf.movieSize.ymax, CEIL) - header.y;

	stream.writeRawData(header.signature, SAM_SIGN_SIZE);
	stream << header.version;
	stream << header.frame_rate;
	stream << header.x;
	stream << header.y;
	stream << header.width;
	stream << header.height;

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMShapes(QDataStream &stream)
{
	Q_ASSERT(shapes.size() <= 65535);
	auto shapeCount = quint16(shapes.size());

	stream << shapeCount;

	if (not outputStreamOk(stream))
		return false;

	for (const Shape &shape : shapes)
	{
		const Image &image = images.at(shape.imageIndex);

		int scaledWidth = qCeil(
				(shape.width * owner->mScale) / TWIPS_PER_PIXELF);
		int scaledHeight = qCeil(
				(shape.height * owner->mScale) / TWIPS_PER_PIXELF);

		int scaledX = scale(shape.matrix.tx, CEIL);
		int scaledY = scale(shape.matrix.ty, CEIL);

		if (scaledWidth <= 0 || scaledWidth > 65535 ||
			scaledHeight <= 0 || scaledHeight > 65535 ||
			scaledX < -32768 || scaledX > 32767 ||
			scaledY < -32768 || scaledY > 32767)
		{
			result = BAD_SCALE_VALUE;
			return false;
		}

		if (not writeSAMString(stream, image.fileName))
			return false;

		stream << quint16(scaledWidth);
		stream << quint16(scaledHeight);
		stream << qint32(shape.matrix.sx);
		stream << qint32(shape.matrix.r1);
		stream << qint32(shape.matrix.r0);
		stream << qint32(shape.matrix.sy);
		stream << qint16(scaledX);
		stream << qint16(scaledY);

		if (not outputStreamOk(stream))
			return false;
	}

	return true;
}

bool Converter::Process::writeSAMFrames(QDataStream &stream)
{
	stream << swf.frameCount;

	if (not outputStreamOk(stream))
		return false;

	for (const Frame &frame : frames)
	{
		auto &removes = frame.removes;
		auto &adds = frame.adds;
		auto &moves = frame.moves;
		auto &labelName = frame.labelName;

		quint8 flags = 0;

		if (not removes.empty())
			flags |= FRAMEFLAGS_REMOVES;

		if (not adds.empty())
			flags |= FRAMEFLAGS_ADDS;

		if (not moves.empty())
			flags |= FRAMEFLAGS_MOVES;

		if (not labelName.isEmpty())
			flags |= FRAMEFLAGS_FRAME_NAME;

		stream << flags;

		if (not outputStreamOk(stream) ||
			not writeSAMFrameRemoves(stream, removes) ||
			not writeSAMFrameAdds(stream, adds) ||
			not writeSAMFrameMoves(stream, moves) ||
			not writeSAMFrameLabel(stream, labelName))
		{
			return false;
		}
	}

	return true;
}

bool Converter::Process::writeSAMString(
	QDataStream &stream, const QString &str)
{
	auto utf8 = str.toUtf8();

	int strLen = utf8.size();
	if (strLen > 65535)
	{
		result = OUTPUT_FILE_WRITE_ERROR;
		return false;
	}

	stream << quint16(strLen);
	stream.writeRawData(utf8.data(), strLen);

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameRemoves(
	QDataStream &stream, const Frame::Removes &removes)
{
	if (removes.empty())
		return true;

	Q_ASSERT(removes.size() <= 255);

	stream << quint8(removes.size());
	for (quint16 depth : removes)
	{
		stream << depth;
	}

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameAdds(
	QDataStream &stream, const Frame::Adds &adds)
{
	if (adds.empty())
		return true;

	Q_ASSERT(adds.size() <= 255);

	stream << quint8(adds.size());

	for (const Frame::ObjectAdd &add : adds)
	{
		stream << add.depth;
		stream << add.shapeId;
	}

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameMoves(
	QDataStream &stream, const Frame::Moves &moves)
{
	if (moves.empty())
		return true;

	Q_ASSERT(moves.size() <= 255);

	stream << quint8(moves.size());
	if (not outputStreamOk(stream))
		return false;

	for (const Frame::ObjectMove &move : moves)
	{
		quint16 depthAndFlags =
			move.depthAndFlags &
			(DEPTH_MASK | MOVEFLAGS_MATRIX | MOVEFLAGS_COLOR);

		int scaledX = 0, scaledY = 0;
		if (depthAndFlags & MOVEFLAGS_MATRIX)
		{
			scaledX = scale(move.matrix.tx, CEIL);
			scaledY = scale(move.matrix.ty, CEIL);

			if (move.matrix.sx == 65536 &&
				move.matrix.sy == 65536 &&
				move.matrix.r0 == 0 &&
				move.matrix.r1 == 0)
			{
				depthAndFlags &= ~MOVEFLAGS_MATRIX;
			}

			if (scaledX > 32767 || scaledX < -32768 ||
				scaledY > 32767 || scaledY < -32768)
			{
				depthAndFlags |= MOVEFLAGS_LONGCOORDS;
			}
		}

		stream << depthAndFlags;
		if (depthAndFlags & MOVEFLAGS_MATRIX)
		{
			stream << qint32(move.matrix.sx);
			stream << qint32(move.matrix.r1);
			stream << qint32(move.matrix.r0);
			stream << qint32(move.matrix.sy);
		}

		if (depthAndFlags & MOVEFLAGS_LONGCOORDS)
		{
			stream << qint32(scaledX);
			stream << qint32(scaledY);
		} else
		{
			stream << qint16(scaledX);
			stream << qint16(scaledY);
		}

		if (depthAndFlags & MOVEFLAGS_COLOR)
		{
			stream << move.color.r;
			stream << move.color.g;
			stream << move.color.b;
			stream << move.color.a;
		}

		if (not outputStreamOk(stream))
			return false;
	}

	return true;
}

bool Converter::Process::writeSAMFrameLabel(
	QDataStream &stream, const QString &labelName)
{
	if (labelName.isEmpty())
		return true;

	return writeSAMString(stream, labelName);
}

bool Converter::Process::outputStreamOk(QDataStream &stream)
{
	if (stream.status() == QDataStream::Ok)
		return true;

	result = OUTPUT_FILE_WRITE_ERROR;
	return false;
}

Converter::Process::Process(Converter *owner)
	: owner(owner)
	, currentFrame(nullptr)
	, jpegTables(nullptr)
	, result(OK)
{
	memset(&swf, 0, sizeof(SWF));

	if (owner->mScale <= 0.1)
	{
		result = BAD_SCALE_VALUE;
		return;
	}

	prefix = owner->outputFilePath(
			QFileInfo(owner->mInputFilePath).baseName());

	readSWF() && parseSWF() && exportSAM();
}

Converter::Process::~Process()
{
	swf_FreeTags(&swf);
}

int Converter::Process::scale(int value, int mode) const
{
	return owner->scale(value, mode);
}
