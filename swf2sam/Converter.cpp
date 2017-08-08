// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses libraries from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#include "Converter.h"

#include "QIODeviceSWFReader.h"

#include "rfxswf.h"

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

#include <memory>

enum
{
	SAM_VERSION_1 = 1,
	SAM_VERSION_2 = 2
};

enum
{
	TWIPS_PER_PIXEL = 20,
	FIXEDTW = 65536 * 20
};

static constexpr const qreal TWIPS_PER_PIXELF = TWIPS_PER_PIXEL;
#define LONG_TO_FLOAT (65536.0)
#define WORD_TO_FLOAT (256.0)

enum
{
	FRAMEFLAGS_REMOVES = 0x01,
	FRAMEFLAGS_ADDS = 0x02,
	FRAMEFLAGS_MOVES = 0x04,
	FRAMEFLAGS_LABEL = 0x08
};

enum
{
	SYMBOLFLAGS_BITMAP = 0x01,
	SYMBOLFLAGS_COLOR = 0x02,
	SYMBOLFLAGS_MATRIX = 0x04,
	SYMBOLFLAGS_SIZE = 0x08
};

enum
{
	MOVEFLAGS_LONGCOORDS = 0x0800,
	MOVEFLAGS_MATRIX = 0x1000,
	MOVEFLAGS_COLOR = 0x2000,
	MOVEFLAGS_ROTATE = 0x4000
};

enum
{
	MOVEFLAGSV2_TRANSFORM = 0x1000,
	MOVEFLAGSV2_COORDS = 0x2000,
	MOVEFLAGSV2_MULTCOLOR = 0x4000,
	MOVEFLAGSV2_ADDCOLOR = 0x8000
};

enum
{
	DEPTHV1_MASK = 0x3FF,
	DEPTHV1_MAX = DEPTHV1_MASK,
	DEPTHV2_MASK = 0xFFF,
	DEPTHV2_MAX = DEPTHV2_MASK
};

enum
{
	SAM_SIGN_SIZE = 4
};

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
	, mSamVersion(SAM_VERSION_2)
	, mResult(OK)
	, mSkipUnsupported(false)
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

	if (not file.open(QFile::ReadOnly))
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

				if (not av.isString())
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
	int width;
	int height;

	QVariant errorInfo;

	QString fileName;

	Image(TAG *tag, TAG *jpegTables, size_t index);

	int id() const;

	int exportImage(const QString &prefix, qreal scale);
};

struct Shape
{
	int imageIndex;

	int width;
	int height;
	MATRIX matrix;
	RGBA color;

	Shape();
};

struct Frame
{
	struct ObjectAdd
	{
		quint16 depth;
		quint16 shapeId;
	};

	struct ObjectMove
	{
		quint16 depth;
		quint16 flags;
		MATRIX matrix;
		RGBA multColor;
		RGBA addColor;

		ObjectMove();
	};

	using Removes = std::vector<quint16>;
	using Adds = std::vector<ObjectAdd>;
	using Moves = std::vector<ObjectMove>;
	using MoveMap = std::map<int, Frame::ObjectMove>;

	QString labelName;

	Removes removes;
	Adds adds;
	Moves moves;
};

static quint8 cxToByte(S16 cx, S16 cadd, double alpha)
{
	if (cx > 256)
		cx = 256;
	else
	if (cx < 0)
		cx = 0;

	qreal result = (cx / WORD_TO_FLOAT) * 255.0;

	if (cadd < 0)
		result += cadd;

	if (result <= 0.0)
		return 0;

	return quint8(result * alpha);
}

static inline quint8 addColorToByte(S16 cadd)
{
	if (cadd > 255)
		cadd = 255;

	if (cadd < 0)
		cadd = 0;

	return quint8(cadd);
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
	inline size_t maxDisplayCount() const;
	inline size_t maxDepth() const;
	inline size_t maxShape() const;

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
	bool writeSAMShapesV1(QDataStream &stream);
	bool writeSAMShapesV2(QDataStream &stream);
	bool writeSAMFrames(QDataStream &stream);
	bool writeSAMString(QDataStream &stream, const QString &str);
	bool writeFrameArrayLength(QDataStream &stream, size_t len);

	bool writeSAMFrameRemoves(
		QDataStream &stream, const Frame::Removes &removes);
	bool writeSAMFrameAdds(QDataStream &stream, const Frame::Adds &adds);
	bool writeSAMFrameMoves(
		QDataStream &stream,
		Frame::Moves &moves,
		Frame::MoveMap &moveMap);
	bool writeSAMFrameMoveV1(
		QDataStream &stream,
		Frame::ObjectMove &move,
		const Frame::ObjectMove &prev);
	bool writeSAMFrameMoveV2(
		QDataStream &stream,
		Frame::ObjectMove &move,
		const Frame::ObjectMove &prev);
	bool writeSAMFrameLabel(QDataStream &stream, const QString &labelName);

	bool outputStreamOk(QDataStream &stream);
};

int Converter::exec()
{
	mWarnings.clear();
	Process process(this);
	mResult = process.result;
	mErrorInfo = process.errorInfo;
	return mResult;
}

static QString fillStyleToStr(int value)
{
	switch (value)
	{
		case 0x00:
			return "SOLID";

		case 0x10:
		case 0x11:
			return "LINEAR_GRADIENT";

		case 0x12:
		case 0x13:
			return "RADIAL_GRADIENT";

		case 0x40:
		case 0x41:
		case 0x42:
		case 0x43:
			return "BITMAP";

		default:
			break;
	}

	return QString("0x%1").arg(value, 2, 16, QChar('0'));
}

QString Converter::warnMessage(const Warning &warn)
{
	switch (warn.code)
	{
		case INPUT_FILE_OPEN_ERROR:
			return "Unable to open SWF file.";

		case INPUT_FILE_FORMAT_ERROR:
			return "SWF file format error.";

		case INPUT_FILE_BAD_DATA_ERROR:
			return QString("Broken SWF file (%1).")
				   .arg(warn.info.toString());

		case UNSUPPORTED_LINESTYLES:
			return "Cannot export line styles to SAM.";

		case UNSUPPORTED_FILLSTYLE:
		{
			auto list = warn.info.toList();
			return QString(
				"Cannot export fill style '%1' "
				"for shape #%2 to SAM.")
				   .arg(fillStyleToStr(list.at(0).toUInt()))
				   .arg(list.at(1).toUInt());
		}

		case UNSUPPORTED_VECTOR_SHAPE:
			return QString(
				"Cannot export shape to SAM "
				"(Vector graphics shape #%1 is unsupported).")
				   .arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_MULTICOLOR_SHAPE:
			return QString(
				"Cannot export shape to SAM "
				"(Multi-color shape #%1 is unsupported).")
				   .arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_MULTIBITMAP_SHAPE:
			return QString(
				"Cannot export shape to SAM "
				"(Multi-bitmap shape #%1 is unsupported).")
				   .arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_NOBITMAP_SHAPE:
			return QString(
				"Cannot export shape to SAM "
				"(No bitmap shape #%1 is unsupported).")
				   .arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_OBJECT_FLAGS:
			return QString("Cannot export object with flags 0x%1 to SAM.")
				   .arg(warn.info.toUInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_OBJECT_DEPTH:
			return QString("Cannot export object with depth %1 to SAM.")
				   .arg(warn.info.toUInt());

		case UNSUPPORTED_SHAPE_COUNT:
			return QString(
				"Cannot export more than %1 shapes to SAM.")
				   .arg(warn.info.toUInt());

		case UNSUPPORTED_DISPLAY_COUNT:
			return QString(
				"Cannot export more than %1 places and/or removes to SAM.")
				   .arg(warn.info.toUInt());

		case UNSUPPORTED_TAG:
		{
			TAG tag;
			tag.id = quint16(warn.info.toInt());
			QString tagName(swf_TagGetName(&tag));

			if (tagName.isEmpty())
				tagName = QString::number(tag.id);

			return QString("Cannot export tag '%1' to SAM.")
				   .arg(tagName);
		}

		case UNKNOWN_IMAGE_ID:
			return QString("Unknown image id %1.")
				   .arg(warn.info.toInt(), 4, 10, QChar('0'));

		case UNKNOWN_SHAPE_ID:
			return QString("Unknown shape id %1.")
				   .arg(warn.info.toInt(), 4, 10, QChar('0'));

		case OUTPUT_DIR_ERROR:
			return "Unable to make output directory.";

		case OUTPUT_FILE_WRITE_ERROR:
			return QString("Unable to write file '%1'.")
				   .arg(QFileInfo(warn.info.toString()).fileName());

		case CONFIG_OPEN_ERROR:
			return "Unable to open configuration file.";

		case CONFIG_PARSE_ERROR:
			return "Unable to parse configuration file.";

		case BAD_SCALE_VALUE:
			return "Bad scale value.";
	}

	return QString();
}

QString Converter::errorMessage() const
{
	QStringList errors;

	for (auto &warn : mWarnings)
	{
		auto message = warnMessage(warn);

		if (not message.isEmpty())
			errors.append(message);
	}

	Warning warn;
	warn.info = mErrorInfo;
	warn.code = mResult;
	QString errorMessage = warnMessage(warn);

	if (not errorMessage.isEmpty())
	{
		errors.append(errorMessage);
	}

	return errors.join('\n');
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
					errorInfo = QString("Bad bits per pixel");
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

	width = scaledWidth;
	height = scaledHeight;

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

Shape::Shape()
	: imageIndex(-1)
	, width(0)
	, height(0)
{
	memset(&matrix, 0, sizeof(MATRIX));
	memset(&color, 0, sizeof(RGBA));
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
		Warning warn;
		warn.info = srcObj.flags;
		warn.code = UNSUPPORTED_OBJECT_FLAGS;

		if (owner->mSkipUnsupported)
		{
			owner->mWarnings.push_back(warn);
		} else
		{
			errorInfo = warn.info;
			result = warn.code;
			return false;
		}
	}

	quint16 depth = srcObj.depth;

	if (depth > maxDepth())
	{
		errorInfo = depth;
		result = UNSUPPORTED_OBJECT_DEPTH;
		return false;
	}

	bool placeObject1 = (tag->id == ST_PLACEOBJECT);

	Frame::ObjectMove move;
	move.flags = 0;

	bool shouldMove = (placeObject1 ||
					   0 != (srcObj.flags & PF_MOVE));

	if (placeObject1 || 0 != (srcObj.flags & PF_CHAR))
	{
		if (shouldMove)
		{
			auto &removes = currentFrame->removes;

			if (removes.size() == maxDisplayCount())
			{
				errorInfo = quint32(removes.size());
				result = UNSUPPORTED_DISPLAY_COUNT;
				return false;
			}

			removes.push_back(depth);
			move.flags |= PF_CHAR;
		}

		auto shapeIt = shapeMap.find(srcObj.id);

		if (shapeIt == shapeMap.end() || shapeIt->second > maxShape())
		{
			errorInfo = srcObj.id;
			result = UNKNOWN_SHAPE_ID;
			return false;
		}

		Frame::ObjectAdd add;
		add.depth = depth;
		add.shapeId = quint16(shapeIt->second);

		auto &adds = currentFrame->adds;

		if (adds.size() == maxDisplayCount())
		{
			errorInfo = quint32(adds.size());
			result = UNSUPPORTED_DISPLAY_COUNT;
			return false;
		}

		adds.push_back(add);
	}

	if (placeObject1 || 0 != (srcObj.flags & PF_CXFORM))
	{
		move.flags |= PF_CXFORM;
	}

	if (placeObject1 || 0 != (srcObj.flags & PF_MATRIX))
	{
		move.flags |= PF_MATRIX;
	}

	if (move.flags != 0)
	{
		move.depth = depth;
		move.matrix = srcObj.matrix;

		double a = 1.0;

		switch (owner->mSamVersion)
		{
			case SAM_VERSION_1:
			{
				int a0 = srcObj.cxform.a0;

				if (a0 > 256)
					a0 = 256;
				else
				if (a0 < 0)
					a0 = 0;

				a = a0 / WORD_TO_FLOAT;
				break;
			}
		}

		move.multColor.a = cxToByte(srcObj.cxform.a0, srcObj.cxform.a1, 1.0);
		move.multColor.r = cxToByte(srcObj.cxform.r0, srcObj.cxform.r1, a);
		move.multColor.g = cxToByte(srcObj.cxform.g0, srcObj.cxform.g1, a);
		move.multColor.b = cxToByte(srcObj.cxform.b0, srcObj.cxform.b1, a);

		move.addColor.a = addColorToByte(srcObj.cxform.a1);
		move.addColor.r = addColorToByte(srcObj.cxform.r1);
		move.addColor.g = addColorToByte(srcObj.cxform.g1);
		move.addColor.b = addColorToByte(srcObj.cxform.b1);

		auto &moves = currentFrame->moves;

		if (moves.size() == maxDisplayCount())
		{
			errorInfo = quint32(moves.size());
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

	if (removes.size() == maxDisplayCount())
	{
		errorInfo = quint32(removes.size());
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
	auto index = shapes.size();

	Warning warn;
	warn.info = QVariantList() << shapeId << quint32(index);

	if (srcShape.numlinestyles > 0)
	{
		warn.code = UNSUPPORTED_LINESTYLES;

		if (not owner->mSkipUnsupported)
		{
			errorInfo = warn.info;
			result = warn.code;
			return false;
		}
	}

	Image *img = nullptr;

	if (index == maxShape())
	{
		errorInfo = quint32(index);
		result = UNSUPPORTED_SHAPE_COUNT;
		return false;
	}

	shapes.push_back(Shape());
	shapeMap[shapeId] = index;
	Shape &shape = shapes.back();
	int colorCount = 0;
	bool multiFill = false;

	for (int i = 0; i < srcShape.numfillstyles; i++)
	{
		auto &fillStyle = srcShape.fillstyles[i];

		switch (fillStyle.type)
		{
			case 0x00:
			{
				shape.color = fillStyle.color;

				if (0 < colorCount++)
				{
					warn.code = UNSUPPORTED_MULTICOLOR_SHAPE;

					if (not owner->mSkipUnsupported)
					{
						errorInfo = warn.info;
						result = warn.code;
						return false;
					}

					multiFill = true;
					owner->mWarnings.push_back(warn);
					break;
				}

				break;
			}

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
					warn.code = UNSUPPORTED_MULTIBITMAP_SHAPE;

					if (not owner->mSkipUnsupported)
					{
						errorInfo = warn.info;
						result = warn.code;
						return false;
					}

					multiFill = true;
					owner->mWarnings.push_back(warn);
					break;
				}

				shape.imageIndex = int(it->second);
				img = &images.at(shape.imageIndex);
				Q_ASSERT(nullptr != img);

				shape.matrix = fillStyle.m;
				break;
			}

			default:
			{
				warn.info = QVariantList()	<< fillStyle.type
											<< shapeId
											<< quint32(index);
				warn.code = UNSUPPORTED_FILLSTYLE;

				if (not owner->mSkipUnsupported)
				{
					errorInfo = warn.info;
					result = warn.code;
					return false;
				}

				owner->mWarnings.push_back(warn);
				break;
			}
		}
	}

	if (nullptr == img)
	{
		warn.code = UNSUPPORTED_NOBITMAP_SHAPE;

		if (owner->mSkipUnsupported)
		{
			owner->mWarnings.push_back(warn);
			return true;
		}

		errorInfo = warn.info;
		result = warn.code;
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

	bool moveZero = line ? line->type == lineTo : false;
	int testLineCount = moveZero ? 3 : 4;

	while (line && ok)
	{
		switch (line->type)
		{
			case lineTo:

				if (line->next == nullptr || lineCount == testLineCount)
				{
					if (lineCount == testLineCount)
					{
						if (moveZero)
						{
							break;
						} else
						if (line->y == srcShape.lines->y &&
							line->x == srcShape.lines->x)
						{
							// total lines valid 4
							// endpoint valid
							break;
						}
					}

					ok = false;
				} else
				{
					curX = line->x;

					if (curX < minX)
						minX = curX;

					if (curX > maxX)
						maxX = curX;

					curY = line->y;

					if (curY < minY)
						minY = curY;

					if (curY > maxY)
						maxY = curY;
				}

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

				if (owner->mSkipUnsupported)
					break;

				// fall through
			}

			default:
				ok = false;
				break;
		}

		if (ok &&
			lineCount == testLineCount &&
			owner->mSkipUnsupported &&
			line->next != nullptr)
		{
			if (multiFill)
				break;

			ok = false;
		}

		if (ok)
		{
			lineCount++;
			line = line->next;
		} else
		{
			warn.code = UNSUPPORTED_VECTOR_SHAPE;

			if (owner->mSkipUnsupported)
			{
				owner->mWarnings.push_back(warn);
				ok = true;
			} else
			{
				errorInfo = warn.info;
				result = warn.code;
			}

			break;
		}
	}

	shape.width = maxX - minX;
	shape.height = maxY - minY;

	if (srcShape.bbox)
	{
		shape.width = srcShape.bbox->xmax - srcShape.bbox->xmin;
		shape.height = srcShape.bbox->ymax - srcShape.bbox->ymin;
	}

	return ok;
}

bool Converter::Process::readSWF()
{
	QFile inputFile(owner->mInputFilePath);

	if (not inputFile.open(QFile::ReadOnly))
	{
		result = INPUT_FILE_OPEN_ERROR;
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
			case ST_DOABC:
			case ST_SYMBOLCLASS:
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
	QFileInfo fileInfo(prefix + ".sam");

	errorInfo = fileInfo.filePath();

	if (not QDir().mkpath(fileInfo.path()))
	{
		result = OUTPUT_DIR_ERROR;
		return false;
	}

	QSaveFile samFile(fileInfo.filePath());

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

	qInfo().noquote() << fileInfo.fileName();
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
	header.version = owner->mSamVersion;
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

	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			return writeSAMShapesV1(stream);

		case SAM_VERSION_2:
			return writeSAMShapesV2(stream);
	}

	return false;
}

bool Converter::Process::writeSAMShapesV1(QDataStream &stream)
{
	for (const Shape &shape : shapes)
	{
		const Image &image = images.at(shape.imageIndex);

		int scaledWidth = image.width;
		int scaledHeight = image.height;

		int scaledX = scale(shape.matrix.tx, CEIL);
		int scaledY = scale(shape.matrix.ty, CEIL);

		if (scaledX < -32768 || scaledX > 32767 ||
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

bool Converter::Process::writeSAMShapesV2(QDataStream &stream)
{
	for (const Shape &shape : shapes)
	{
		quint8 flags = 0;

		int scaledWidth;
		int scaledHeight;

		if (shape.imageIndex >= 0)
		{
			Q_ASSERT(shape.imageIndex <= 0xFFFF);
			flags |= SYMBOLFLAGS_BITMAP;
			auto &image = images.at(shape.imageIndex);
			scaledWidth = image.width;
			scaledHeight = image.height;
		} else
		{
			scaledWidth =
				qCeil((shape.width / TWIPS_PER_PIXELF) * owner->mScale);
			scaledHeight =
				qCeil((shape.height / TWIPS_PER_PIXELF) * owner->mScale);
		}

		if (scaledWidth < 0 || scaledHeight < 0 ||
			scaledWidth > 65535 || scaledHeight > 65535)
		{
			result = BAD_SCALE_VALUE;
			return false;
		}

		if (scaledWidth >= 0 || scaledHeight >= 0)
		{
			flags |= SYMBOLFLAGS_SIZE;
		}

		if (shape.color.a > 0)
		{
			flags |= SYMBOLFLAGS_COLOR;
		}

		if (shape.matrix.tx != 0 || shape.matrix.ty != 0 ||
			shape.matrix.r0 != 0 || shape.matrix.r1 != 0 ||
			shape.matrix.sx != FIXEDTW || shape.matrix.sy != FIXEDTW)
		{
			flags |= SYMBOLFLAGS_MATRIX;
		}

		stream << flags;

		if (flags & SYMBOLFLAGS_BITMAP)
		{
			stream << quint16(shape.imageIndex);
		}

		if (flags & SYMBOLFLAGS_COLOR)
		{
			stream << shape.color.r;
			stream << shape.color.g;
			stream << shape.color.b;
			stream << shape.color.a;
		}

		if (flags & SYMBOLFLAGS_SIZE)
		{
			stream << quint16(scaledWidth);
			stream << quint16(scaledHeight);
		}

		if (flags & SYMBOLFLAGS_MATRIX)
		{
			int scaledX = scale(shape.matrix.tx, CEIL);
			int scaledY = scale(shape.matrix.ty, CEIL);
			stream << qint32(shape.matrix.sx / TWIPS_PER_PIXEL);
			stream << qint32(shape.matrix.r1);
			stream << qint32(shape.matrix.r0);
			stream << qint32(shape.matrix.sy / TWIPS_PER_PIXEL);
			stream << qint32(scaledX);
			stream << qint32(scaledY);
		}

		if (not outputStreamOk(stream))
			return false;
	}

	return true;
}

bool Converter::Process::writeSAMFrames(QDataStream &stream)
{
	stream << quint16(swf.frameCount);

	if (not outputStreamOk(stream))
		return false;

	Frame::MoveMap moveMap;

	for (const Frame &frame : frames)
	{
		auto &removes = frame.removes;
		auto &adds = frame.adds;
		auto moves = frame.moves;

		for (auto remove : removes)
		{
			bool found = false;

			for (auto &move : moves)
			{
				if (move.depth == remove &&
					0 != (move.flags & PF_CHAR))
				{
					found = true;
					break;
				}
			}

			if (not found)
				moveMap.erase(remove);
		}

		auto &labelName = frame.labelName;

		quint8 flags = 0;

		if (not removes.empty())
			flags |= FRAMEFLAGS_REMOVES;

		if (not adds.empty())
			flags |= FRAMEFLAGS_ADDS;

		if (not moves.empty())
			flags |= FRAMEFLAGS_MOVES;

		if (not labelName.isEmpty())
			flags |= FRAMEFLAGS_LABEL;

		stream << flags;

		if (not outputStreamOk(stream) ||
			not writeSAMFrameRemoves(stream, removes) ||
			not writeSAMFrameAdds(stream, adds) ||
			not writeSAMFrameMoves(stream, moves, moveMap) ||
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

bool Converter::Process::writeFrameArrayLength(QDataStream &stream, size_t len)
{
	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			Q_ASSERT(len <= 255);
			stream << quint8(len);
			break;

		case SAM_VERSION_2:
			Q_ASSERT(len <= 65535);
			stream << quint16(len);
			break;

		default:
			return false;
	}

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameRemoves(
	QDataStream &stream, const Frame::Removes &removes)
{
	if (removes.empty())
		return true;

	if (not writeFrameArrayLength(stream, removes.size()))
		return false;

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

	if (not writeFrameArrayLength(stream, adds.size()))
		return false;

	for (const Frame::ObjectAdd &add : adds)
	{
		stream << quint16(add.depth);

		switch (owner->mSamVersion)
		{
			case SAM_VERSION_1:
				stream << quint8(add.shapeId);
				break;

			case SAM_VERSION_2:
				stream << quint16(add.shapeId);
				break;

			default:
				return false;
		}
	}

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameMoves(
	QDataStream &stream,
	Frame::Moves &moves,
	Frame::MoveMap &moveMap)
{
	if (moves.empty())
		return true;

	if (not writeFrameArrayLength(stream, moves.size()))
		return false;

	for (Frame::ObjectMove &move : moves)
	{
		Frame::ObjectMove prev;

		auto it = moveMap.find(move.depth);

		if (it != moveMap.end())
			prev = it->second;

		switch (owner->mSamVersion)
		{
			case SAM_VERSION_1:

				if (not writeSAMFrameMoveV1(stream, move, prev))
					return false;

				break;

			case SAM_VERSION_2:

				if (not writeSAMFrameMoveV2(stream, move, prev))
					return false;

				break;

			default:
				return false;
		}

		moveMap[move.depth] = move;
	}

	return true;
}

bool Converter::Process::writeSAMFrameMoveV1(
	QDataStream &stream,
	Frame::ObjectMove &move,
	const Frame::ObjectMove &prev)
{
	Q_ASSERT(move.depth <= DEPTHV1_MAX);
	quint16 depthAndFlags = move.depth & DEPTHV1_MASK;

	if (0 == (move.flags & PF_MATRIX))
	{
		move.matrix = prev.matrix;
	}

	if (0 == (move.flags & PF_CXFORM))
	{
		move.multColor = prev.multColor;
		move.addColor = prev.addColor;
	}

	if (move.matrix.sx != 65536 ||
		move.matrix.sy != 65536 ||
		move.matrix.r0 != 0 ||
		move.matrix.r1 != 0)
	{
		depthAndFlags |= MOVEFLAGS_MATRIX;
	}

	int scaledX = scale(move.matrix.tx, CEIL);
	int scaledY = scale(move.matrix.ty, CEIL);

	if (scaledX > 32767 || scaledX < -32768 ||
		scaledY > 32767 || scaledY < -32768)
	{
		depthAndFlags |= MOVEFLAGS_LONGCOORDS;
	}

	{
		RGBA tempMultColor = { 255, 255, 255, 255 };

		if (0 == (move.flags & PF_CHAR))
		{
			tempMultColor = prev.multColor;
		}

		if (move.flags & (PF_CXFORM | PF_CHAR))
		{
			if (0 != memcmp(&move.multColor, &tempMultColor, sizeof(RGBA)))
				depthAndFlags |= MOVEFLAGS_COLOR;
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
		stream << move.multColor.r;
		stream << move.multColor.g;
		stream << move.multColor.b;
		stream << move.multColor.a;
	}

	return outputStreamOk(stream);
}

bool Converter::Process::writeSAMFrameMoveV2(
	QDataStream &stream,
	Frame::ObjectMove &move,
	const Frame::ObjectMove &prev)
{
	Q_ASSERT(move.depth <= DEPTHV2_MAX);
	quint16 depthAndFlags = move.depth & DEPTHV2_MASK;

	int scaledX = 0, scaledY = 0;

	if (0 == (move.flags & PF_MATRIX))
	{
		move.matrix = prev.matrix;
	}

	if (0 == (move.flags & PF_CXFORM))
	{
		move.multColor = prev.multColor;
		move.addColor = prev.addColor;
	}

	{
		Frame::ObjectMove temp;

		if (0 == (move.flags & PF_CHAR))
			temp = prev;

		if (move.flags & (PF_MATRIX | PF_CHAR))
		{
			if (move.matrix.sx != temp.matrix.sx ||
				move.matrix.sy != temp.matrix.sy ||
				move.matrix.r0 != temp.matrix.r0 ||
				move.matrix.r1 != temp.matrix.r1)
			{
				depthAndFlags |= MOVEFLAGSV2_TRANSFORM;
			}

			if (move.matrix.tx != temp.matrix.tx ||
				move.matrix.ty != temp.matrix.ty)
			{
				scaledX = scale(move.matrix.tx, CEIL);
				scaledY = scale(move.matrix.ty, CEIL);
				depthAndFlags |= MOVEFLAGSV2_COORDS;
			}
		}

		if (move.flags & (PF_CXFORM | PF_CHAR))
		{
			if (0 != memcmp(&move.multColor, &temp.multColor, sizeof(RGBA)))
				depthAndFlags |= MOVEFLAGSV2_MULTCOLOR;

			if (0 != memcmp(&move.addColor, &temp.addColor, sizeof(RGBA)))
				depthAndFlags |= MOVEFLAGSV2_ADDCOLOR;
		}
	}

	stream << depthAndFlags;

	if (depthAndFlags & MOVEFLAGSV2_TRANSFORM)
	{
		stream << qint32(move.matrix.sx);
		stream << qint32(move.matrix.r1);
		stream << qint32(move.matrix.r0);
		stream << qint32(move.matrix.sy);
	}

	if (depthAndFlags & MOVEFLAGSV2_COORDS)
	{
		stream << qint32(scaledX);
		stream << qint32(scaledY);
	}

	if (depthAndFlags & MOVEFLAGSV2_MULTCOLOR)
	{
		stream << move.multColor.r;
		stream << move.multColor.g;
		stream << move.multColor.b;
		stream << move.multColor.a;
	}

	if (depthAndFlags & MOVEFLAGSV2_ADDCOLOR)
	{
		stream << move.addColor.r;
		stream << move.addColor.g;
		stream << move.addColor.b;
		stream << move.addColor.a;
	}

	return outputStreamOk(stream);
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

	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
		case SAM_VERSION_2:
			break;

		default:
			result = BAD_SAM_VERSION;
			return;
	}

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

size_t Converter::Process::maxDisplayCount() const
{
	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			return 0xFF;

		case SAM_VERSION_2:
			return 0xFFFF;
	}

	return 0;
}

size_t Converter::Process::maxDepth() const
{
	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			return DEPTHV1_MAX;

		case SAM_VERSION_2:
			return DEPTHV2_MAX;
	}

	return 0;
}

size_t Converter::Process::maxShape() const
{
	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			return 0xFF;

		case SAM_VERSION_2:
			return 0xFFFF;
	}

	return 0;
}

Frame::ObjectMove::ObjectMove()
	: depth(0)
	, flags(0)
{
	swf_GetMatrix(nullptr, &matrix);
	memset(&multColor, 255, sizeof(RGBA));
	memset(&addColor, 0, sizeof(RGBA));
}
