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
#include <functional>
#include <algorithm>
#include <set>

enum
{
	SAM_VERSION_1 = 1,
	SAM_VERSION_2 = 2
};

enum
{
	TWIPS_PER_PIXEL = 20,
	FIXEDTW = 65536 * TWIPS_PER_PIXEL
};

static Q_CONSTEXPR qreal TWIPS_PER_PIXELF = TWIPS_PER_PIXEL;
static Q_CONSTEXPR qreal WORD_TO_FLOAT = 256.0;

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

	QString filePathForPrefix(const QString &prefix) const;

	Image(TAG *tag, TAG *jpegTables, size_t index);

	int id() const;

	int exportImage(const QString &prefix, qreal scale);
};

struct Shape
{
	int imageIndex;
	QPolygon vertices;
	MATRIX matrix;
	RGBA color;

	Shape();

	bool isRect() const;
};

struct ShapeRef
{
	size_t startIndex;
	size_t endIndex;

	size_t shapeCount() const;
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

	struct DepthRef
	{
		size_t startDepth;
		size_t endDepth;
	};

	using Removes = std::vector<quint16>;
	using RemoveSet = std::set<quint16>;
	using Adds = std::vector<ObjectAdd>;
	using Moves = std::vector<ObjectMove>;
	using MoveMap = std::map<int, Frame::ObjectMove>;
	using DepthMap = std::map<int, DepthRef>;

	QString labelName;

	Removes removes;
	Adds adds;
	Moves moves;
};

static quint8 cxToByte(S16 cx, S16 cadd, double alpha)
{
	if (cx > 256)
		cx = 256;
	else if (cx < 0)
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
	std::vector<ShapeRef> shapeRefs;

	std::map<int, size_t> imageMap;
	std::map<int, size_t> shapeRefMap;

	LabelRenameMap renames;

	QString prefix;
	QVariant errorInfo;

	Converter *owner;
	Frame *currentFrame;
	TAG *jpegTables;
	int result;
	quint16 firstDepth;
	quint8 depthMultiplier;

	class SAMWriter
	{
		Process &owner;
		QDataStream stream;
		Frame::RemoveSet removes;
		Frame::Adds adds;
		Frame::Moves moves;
		Frame::MoveMap moveMap;
		Frame::DepthMap depthMap;

	public:
		SAMWriter(Process &owner, QIODevice *device);

		bool exec();

	private:
		bool writeHeader();
		bool writeShapes();
		bool writeShapesV1();
		bool writeShapesV2();
		bool writeFrames();
		bool writeString(const QString &str);
		bool writeDisplayCount(size_t len);

		bool prepareObjectRemoves(const Frame &frame);
		bool writeObjectRemoves();

		bool prepareObjectAdds(const Frame &frame);
		bool writeObjectAdds();

		bool prepareObjectMoves(const Frame &frame);
		bool writeObjectMoves();
		bool writeObjectMoveV1(
			Frame::ObjectMove &move, const Frame::ObjectMove &prev);
		bool writeObjectMoveV2(
			Frame::ObjectMove &move, const Frame::ObjectMove &prev);
		bool writeFrameLabel(const Frame &frame);
		bool writeFrameFlags(const Frame &frame);
		bool writeFrameCount();

		bool outputStreamOk();
	};

	Process(Converter *owner);
	~Process();

	inline int scale(int value, int mode) const;
	size_t maxDisplayCount() const;
	size_t maxDepth() const;
	size_t maxShape() const;

	bool handleShowFrame();
	bool handleFrameLabel(TAG *tag);
	bool handlePlaceObject(TAG *tag);
	bool handleRemoveObject(TAG *tag);
	bool handleImage(TAG *tag);
	bool handleShape(TAG *tag);
	bool readSWF();
	bool parseSWF();
	bool exportSAM();
};

Shape::Shape()
	: imageIndex(-1)
{
	memset(&matrix, 0, sizeof(matrix));
	memset(&color, 0, sizeof(color));
	matrix.sx = FIXEDTW;
	matrix.sy = FIXEDTW;
}

bool Shape::isRect() const
{
	if (vertices.isEmpty())
		return false;

	int vertexCount = 4 + ((vertices.first() == vertices.last()) ? 1 : 0);

	if (vertexCount != vertices.count())
		return false;

	auto &p1 = vertices.at(0);
	auto &p2 = vertices.at(1);
	auto &p3 = vertices.at(2);
	auto &p4 = vertices.at(3);

	return (p1 - p2 == p4 - p3) && (p4 - p1 == p3 - p2);
}

size_t ShapeRef::shapeCount() const
{
	if (endIndex < startIndex)
		return 0;

	return (endIndex - startIndex) + 1;
}

int Converter::exec()
{
	mWarnings.clear();
	Process process(this);
	mResult = process.result;
	mErrorInfo = process.errorInfo;
	return mResult;
}

QString Converter::tagName(const QVariant &t)
{
	return tagName(quint16(t.toUInt()));
}

QString Converter::tagName(quint16 t)
{
	TAG tag;
	tag.id = t;
	QString tagName(swf_TagGetName(&tag));

	if (tagName.isEmpty())
		tagName = QString::number(tag.id);

	return tagName;
}

QString Converter::fillStyleToStr(int value)
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
			return QString("Broken SWF file (%1).").arg(warn.info.toString());

		case UNSUPPORTED_LINESTYLES:
			return "Cannot export line styles to SAM.";

		case UNSUPPORTED_FILLSTYLE:
		{
			auto list = warn.info.toList();
			return QString("Cannot export fill style '%1' "
						   "for shape #%2 to SAM.")
				.arg(fillStyleToStr(list.at(0).toInt()))
				.arg(list.at(1).toUInt());
		}

		case UNSUPPORTED_VECTOR_SHAPE:
			return QString("Cannot export shape to SAM "
						   "(Vector graphics shape #%1 is unsupported).")
				.arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_NOBITMAP_SHAPE:
			return QString("Cannot export shape to SAM "
						   "(No bitmap shape #%1 is unsupported).")
				.arg(warn.info.toList().at(0).toUInt());

		case UNSUPPORTED_OBJECT_FLAGS:
			return QString("Cannot export object with flags 0x%1 to SAM.")
				.arg(warn.info.toUInt(), 4, 16, QChar('0'));

		case UNSUPPORTED_OBJECT_DEPTH:
			return QString("Cannot export object with depth %1 to SAM.")
				.arg(warn.info.toUInt());

		case UNSUPPORTED_SHAPE_COUNT:
			return QString("Cannot export more than %1 shapes to SAM.")
				.arg(warn.info.toUInt());

		case UNSUPPORTED_DISPLAY_COUNT:
			return QString(
				"Cannot export more than %1 places and/or removes to SAM.")
				.arg(warn.info.toUInt());

		case UNSUPPORTED_TAG:
		{
			return QString("Cannot export tag '%1' to SAM.")
				.arg(tagName(warn.info));
		}

		case UNKNOWN_IMAGE_ID:
			return QString("Unknown image id %1.")
				.arg(warn.info.toUInt(), 4, 10, QChar('0'));

		case UNKNOWN_SHAPE_ID:
			return QString("Unknown shape id %1.")
				.arg(warn.info.toUInt(), 4, 10, QChar('0'));

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

QString Image::filePathForPrefix(const QString &prefix) const
{
	static const QString nameFmt("%1%2.png");

	return nameFmt.arg(prefix).arg(index, 4, 10, QChar('0'));
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
		if (data[t] == 0xff && data[t + 1] == 0xd9 && data[t + 2] == 0xff &&
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

	if (Z_OK !=
		uncompress(reinterpret_cast<Bytef *>(result.data()), &destlen,
			&t->data[t->pos], t->len - t->pos))
	{
		return QByteArray();
	}

	result.resize(int(destlen));
	return result;
}

int Image::exportImage(const QString &prefix, qreal scale)
{
	auto imageFilePath = filePathForPrefix(prefix);

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

			if (writeLen > 0 &&
				jpegBuffer.write(reinterpret_cast<char *>(&tag->data[skip]),
					writeLen) != writeLen)
			{
				return Converter::OUTPUT_FILE_WRITE_ERROR;
			}

			jpegBuffer.close();

			auto &bytes = jpegBuffer.buffer();

			image = QImage::fromData(
				reinterpret_cast<const quint8 *>(bytes.constData()),
				bytes.count());

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

					if (Z_OK !=
						uncompress(data.get(), &uncompressedSize,
							&tag->data[end], uLong(compressedAlphaSize)))
					{
						errorInfo = QString("Jpeg alpha failed");
						return Converter::INPUT_FILE_BAD_DATA_ERROR;
					}

					if (uncompressedSize != uLong(alphaSize))
					{
						errorInfo = QString("Jpeg alpha failed");
						return Converter::INPUT_FILE_BAD_DATA_ERROR;
					}

					image = image.convertToFormat(
						QImage::Format_RGBA8888_Premultiplied);

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
			swf_GetU16(tag); // skip index
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
					image = QImage(width, height,
						alpha ? QImage::Format_RGBA8888_Premultiplied
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

	if (scaledWidth <= 0 || scaledWidth > 16386 || scaledHeight <= 0 ||
		scaledHeight > 16386)
	{
		return Converter::BAD_SCALE_VALUE;
	}

	if (scaledWidth != image.width() || scaledHeight != image.height())
	{
		image = image.scaled(scaledWidth, scaledHeight,
			Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
	}

	if (not QDir().mkpath(QFileInfo(prefix).path()))
	{
		return Converter::OUTPUT_DIR_ERROR;
	}

	errorInfo = imageFilePath;

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
	auto labelName = swf.fileVersion >= 6 ? QString::fromUtf8(str)
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

	if (srcObj.flags & ~(PF_CHAR | PF_CXFORM | PF_MATRIX | PF_MOVE | PF_NAME))
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

	bool placeObject1 = (tag->id == ST_PLACEOBJECT);

	Frame::ObjectMove move;
	move.flags = 0;

	bool shouldMove = (placeObject1 || 0 != (srcObj.flags & PF_MOVE));

	if (placeObject1 || 0 != (srcObj.flags & PF_CHAR))
	{
		if (shouldMove)
		{
			currentFrame->removes.push_back(depth);
			move.flags |= PF_CHAR;
		}

		auto shapeRefIt = shapeRefMap.find(srcObj.id);

		if (shapeRefIt == shapeRefMap.end())
		{
			errorInfo = srcObj.id;
			result = UNKNOWN_SHAPE_ID;
			return false;
		}

		Frame::ObjectAdd add;
		add.depth = depth;
		add.shapeId = quint16(shapeRefIt->second);

		if (depth < firstDepth)
			firstDepth = depth;

		currentFrame->adds.push_back(add);
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
				{
					a0 = 256;
				} else if (a0 < 0)
				{
					a0 = 0;
				}

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

		currentFrame->moves.push_back(move);
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

	currentFrame->removes.push_back(quint16(swf_GetDepth(tag)));

	return true;
}

bool Converter::Process::handleImage(TAG *tag)
{
	auto prefix = this->prefix;

	switch (owner->mSamVersion)
	{
		case SAM_VERSION_1:
			prefix += '_';
			break;

		case SAM_VERSION_2:
			prefix += '/';
			break;

		default:
			return false;
	}

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
		case OUTPUT_FILE_WRITE_ERROR:
			errorInfo = image.errorInfo;
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
	size_t index = shapeRefs.size();
	size_t shapeIndex = shapes.size();

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

		owner->mWarnings.push_back(warn);
	}

	shapeRefs.emplace_back();
	shapeRefMap[shapeId] = index;
	ShapeRef &shapeRef = shapeRefs.back();

	shapeRef.startIndex = shapeIndex;
	shapeRef.endIndex = shapeIndex - 1;

	Image *img = nullptr;

	std::map<int, size_t> fillStyleMap;

	for (int i = 0; i < srcShape.numfillstyles; i++)
	{
		auto &fillStyle = srcShape.fillstyles[i];

		switch (fillStyle.type)
		{
			case 0x40: // BITMAP FILL
			case 0x41:
			case 0x42:
			case 0x43:
			{
				int imageId = fillStyle.id_bitmap;

				if (imageId == 65535)
					continue;

				fillStyleMap[i + 1] = shapes.size();

				shapes.emplace_back();
				Shape &shape = shapes.back();

				auto it = imageMap.find(imageId);

				if (it == imageMap.end())
				{
					errorInfo = imageId;
					result = UNKNOWN_IMAGE_ID;
					return false;
				}

				shape.imageIndex = int(it->second);
				img = &images.at(shape.imageIndex);
				Q_ASSERT(nullptr != img);

				shape.matrix = fillStyle.m;
				shapeRef.endIndex++;

				break;
			}

			case 0x00: // SOLID FILL
			{
				if (owner->mSamVersion != SAM_VERSION_1)
				{
					fillStyleMap[i + 1] = shapes.size();

					shapes.emplace_back();
					Shape &shape = shapes.back();

					shape.color = fillStyle.color;
					shapeRef.endIndex++;

					break;
				}
			} // fall through

			default:
			{
				warn.info = QVariantList()
					<< fillStyle.type << shapeId << quint32(index);
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

	if (nullptr == img && owner->mSamVersion == SAM_VERSION_1)
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

	bool ok = true;

	while (line && ok)
	{
		int fs0 = line->fillstyle0;
		int fs1 = line->fillstyle1;
		if (fs0 != 0)
		{
			std::swap(fs0, fs1);
		}

		if (fs0 != 0 && not owner->mSkipUnsupported)
		{
			ok = false;
			break;
		}

		auto it = fillStyleMap.find(fs1);

		if (it != fillStyleMap.end())
		{
			auto &poly = shapes.at(it->second).vertices;

			if (poly.isEmpty() && line->type == lineTo)
			{
				poly.append(QPoint());
			}

			switch (line->type)
			{
				case moveTo:
				{
					if (not poly.isEmpty())
					{
						if (not owner->mSkipUnsupported)
						{
							ok = false;
						}

						break;
					}

					// fall through
				}

				case lineTo:
				{
					poly.append(QPoint(line->x, line->y));
					break;
				}

				default:
					ok = false;
					break;
			}
		}

		if (ok)
		{
			line = line->next;
		}
	}

	for (; shapeIndex <= shapeRef.endIndex; shapeIndex++)
	{
		if (not shapes.at(shapeIndex).isRect())
		{
			ok = false;
			break;
		}
	}

	if (not ok)
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
	}

	auto shapeCount = shapeRef.shapeCount();

	if (shapeCount > 255)
	{
		errorInfo = quint32(shapeCount);
		result = UNSUPPORTED_SHAPE_COUNT;
		return false;
	}

	if (shapeCount > depthMultiplier)
		depthMultiplier = quint8(shapeCount);

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
		SAMWriter writer(*this, &samFile);

		if (not writer.exec())
			return false;
	} // close writer

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

Converter::Process::SAMWriter::SAMWriter(Process &owner, QIODevice *device)
	: owner(owner)
	, stream(device)
{
	stream.setByteOrder(QDataStream::LittleEndian);
}

bool Converter::Process::SAMWriter::exec()
{
	return writeHeader() && writeShapes() && writeFrames();
}

bool Converter::Process::SAMWriter::writeHeader()
{
	auto &swf = owner.swf;
	SAM_Header header;
	memcpy(header.signature, SAM_Signature, SAM_SIGN_SIZE);
	header.version = owner.owner->mSamVersion;
	header.frame_rate = quint8(swf.frameRate >> 8);
	header.x = owner.scale(swf.movieSize.xmin, FLOOR);
	header.y = owner.scale(swf.movieSize.ymin, FLOOR);
	header.width = owner.scale(swf.movieSize.xmax, CEIL) - header.x;
	header.height = owner.scale(swf.movieSize.ymax, CEIL) - header.y;

	stream.writeRawData(header.signature, SAM_SIGN_SIZE);
	stream << header.version;
	stream << header.frame_rate;
	stream << header.x;
	stream << header.y;
	stream << header.width;
	stream << header.height;

	if (not outputStreamOk())
		return false;

	switch (header.version)
	{
		case SAM_VERSION_1:
			return true;

		case SAM_VERSION_2:
			return writeString(QFileInfo(owner.prefix).fileName());
	}

	return false;
}

bool Converter::Process::SAMWriter::writeShapes()
{
	auto &shapes = owner.shapes;

	if (shapes.size() > owner.maxShape())
	{
		owner.errorInfo = quint32(shapes.size());
		owner.result = UNSUPPORTED_SHAPE_COUNT;
		return false;
	}

	Q_ASSERT(shapes.size() <= 65535);
	auto shapeCount = quint16(shapes.size());

	stream << shapeCount;

	if (not outputStreamOk())
		return false;

	switch (owner.owner->mSamVersion)
	{
		case SAM_VERSION_1:
			return writeShapesV1();

		case SAM_VERSION_2:
			return writeShapesV2();
	}

	return false;
}

bool Converter::Process::SAMWriter::writeShapesV1()
{
	for (const Shape &shape : owner.shapes)
	{
		const Image &image = owner.images.at(shape.imageIndex);

		int scaledWidth = image.width;
		int scaledHeight = image.height;

		int scaledX = owner.scale(shape.matrix.tx, CEIL);
		int scaledY = owner.scale(shape.matrix.ty, CEIL);

		if (scaledX < -32768 || scaledX > 32767 || scaledY < -32768 ||
			scaledY > 32767)
		{
			owner.result = BAD_SCALE_VALUE;
			return false;
		}

		if (not writeString(image.fileName))
			return false;

		stream << quint16(scaledWidth);
		stream << quint16(scaledHeight);
		stream << qint32(shape.matrix.sx);
		stream << qint32(shape.matrix.r1);
		stream << qint32(shape.matrix.r0);
		stream << qint32(shape.matrix.sy);
		stream << qint16(scaledX);
		stream << qint16(scaledY);

		if (not outputStreamOk())
			return false;
	}

	return true;
}

bool Converter::Process::SAMWriter::writeShapesV2()
{
	for (const Shape &shape : owner.shapes)
	{
		quint8 flags = 0;

		int scaledWidth;
		int scaledHeight;

		if (shape.imageIndex >= 0)
		{
			Q_ASSERT(shape.imageIndex <= 0xFFFF);
			flags |= SYMBOLFLAGS_BITMAP;
			auto &image = owner.images.at(shape.imageIndex);
			scaledWidth = image.width;
			scaledHeight = image.height;
		} else
		{
			auto bb = shape.vertices.boundingRect();
			qreal scale = owner.owner->mScale;

			scaledWidth = qCeil((bb.width() / TWIPS_PER_PIXELF) * scale);
			scaledHeight = qCeil((bb.height() / TWIPS_PER_PIXELF) * scale);
		}

		if (scaledWidth < 0 || scaledHeight < 0 || scaledWidth > 65535 ||
			scaledHeight > 65535)
		{
			owner.result = BAD_SCALE_VALUE;
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
			int scaledX = owner.scale(shape.matrix.tx, CEIL);
			int scaledY = owner.scale(shape.matrix.ty, CEIL);
			stream << qint32(qRound(shape.matrix.sx / TWIPS_PER_PIXELF));
			stream << qint32(qRound(shape.matrix.r1 / TWIPS_PER_PIXELF));
			stream << qint32(qRound(shape.matrix.r0 / TWIPS_PER_PIXELF));
			stream << qint32(qRound(shape.matrix.sy / TWIPS_PER_PIXELF));
			stream << qint32(scaledX);
			stream << qint32(scaledY);
		}

		if (not outputStreamOk())
			return false;
	}

	return true;
}

bool Converter::Process::SAMWriter::prepareObjectRemoves(const Frame &frame)
{
	removes.clear();

	for (quint16 removeDepth : frame.removes)
	{
		auto it = depthMap.find(removeDepth);

		if (it == depthMap.end())
		{
			continue;
		}

		auto &depthRef = it->second;

		for (size_t depth = depthRef.startDepth; depth <= depthRef.endDepth;
			 depth++)
		{
			if (depth > owner.maxDepth())
			{
				owner.errorInfo = quint32(depth);
				owner.result = UNSUPPORTED_OBJECT_DEPTH;
				return false;
			}

			removes.insert(quint16(depth));
		}
	}

	return true;
}

bool Converter::Process::SAMWriter::writeFrames()
{
	depthMap.clear();
	moveMap.clear();

	if (not writeFrameCount())
		return false;

	for (const auto &frame : owner.frames)
	{
		if (not prepareObjectRemoves(frame) || not prepareObjectAdds(frame) ||
			not prepareObjectMoves(frame) || not writeFrameFlags(frame) ||
			not writeObjectRemoves() || not writeObjectAdds() ||
			not writeObjectMoves() || not writeFrameLabel(frame))
		{
			return false;
		}
	}

	return true;
}

bool Converter::Process::SAMWriter::writeString(const QString &str)
{
	auto utf8 = str.toUtf8();

	int strLen = utf8.size();

	if (strLen > 65535)
	{
		owner.result = OUTPUT_FILE_WRITE_ERROR;
		return false;
	}

	stream << quint16(strLen);
	stream.writeRawData(utf8.data(), strLen);

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::writeDisplayCount(size_t len)
{
	if (len > owner.maxDisplayCount())
	{
		owner.errorInfo = quint32(len);
		owner.result = UNSUPPORTED_DISPLAY_COUNT;
		return false;
	}

	switch (owner.owner->mSamVersion)
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

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::writeObjectRemoves()
{
	if (removes.empty())
		return true;

	if (not writeDisplayCount(removes.size()))
		return false;

	for (quint16 depth : removes)
	{
		stream << depth;
	}

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::prepareObjectAdds(const Frame &frame)
{
	adds.clear();

	for (auto &add : frame.adds)
	{
		const auto &shapeRef = owner.shapeRefs.at(add.shapeId);

		auto &depthRef = depthMap[add.depth];
		size_t depth =
			size_t(add.depth - owner.firstDepth) * owner.depthMultiplier;
		depthRef.startDepth = depth;
		depthRef.endDepth = depth - 1;

		for (size_t shapeIndex = shapeRef.startIndex;
			 shapeIndex <= shapeRef.endIndex; shapeIndex++, depth++)
		{
			if (depth > owner.maxDepth())
			{
				owner.errorInfo = quint32(depth);
				owner.result = UNSUPPORTED_OBJECT_DEPTH;
				return false;
			}

			adds.emplace_back();
			auto &newAdd = adds.back();
			newAdd.depth = quint16(depth);
			newAdd.shapeId = quint16(shapeIndex);
			depthRef.endDepth++;
		}
	}

	return true;
}

bool Converter::Process::SAMWriter::writeObjectAdds()
{
	if (adds.empty())
		return true;

	if (not writeDisplayCount(adds.size()))
		return false;

	for (const Frame::ObjectAdd &add : adds)
	{
		stream << quint16(add.depth);

		switch (owner.owner->mSamVersion)
		{
			case SAM_VERSION_1:
				Q_ASSERT(add.shapeId <= 255);
				stream << quint8(add.shapeId);
				break;

			case SAM_VERSION_2:
				Q_ASSERT(add.shapeId <= 65535);
				stream << quint16(add.shapeId);
				break;

			default:
				return false;
		}
	}

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::prepareObjectMoves(const Frame &frame)
{
	moves.clear();

	for (auto &move : frame.moves)
	{
		auto it = depthMap.find(move.depth);

		if (it == depthMap.end())
		{
			continue;
		}

		auto &depthRef = it->second;

		for (size_t depth = depthRef.startDepth; depth <= depthRef.endDepth;
			 depth++)
		{
			if (depth > owner.maxDepth())
			{
				owner.errorInfo = quint32(depth);
				owner.result = UNSUPPORTED_OBJECT_DEPTH;
				return false;
			}

			moves.push_back(move);
			auto &newMove = moves.back();
			newMove.depth = quint16(depth);
		}
	}

	auto tempRemoves = removes;
	for (auto &move : moves)
	{
		auto it = tempRemoves.find(move.depth);
		if (it != tempRemoves.end() && 0 != (move.flags & PF_CHAR))
		{
			tempRemoves.erase(it);
		}
	}

	for (quint16 removeDepth : tempRemoves)
	{
		moveMap.erase(removeDepth);
	}

	return true;
}

bool Converter::Process::SAMWriter::writeObjectMoves()
{
	if (moves.empty())
		return true;

	if (not writeDisplayCount(moves.size()))
		return false;

	for (Frame::ObjectMove &move : moves)
	{
		Frame::ObjectMove prev;

		auto it = moveMap.find(move.depth);

		if (it != moveMap.end())
			prev = it->second;

		switch (owner.owner->mSamVersion)
		{
			case SAM_VERSION_1:

				if (not writeObjectMoveV1(move, prev))
					return false;

				break;

			case SAM_VERSION_2:

				if (not writeObjectMoveV2(move, prev))
					return false;

				break;

			default:
				return false;
		}

		moveMap[move.depth] = move;
	}

	return true;
}

bool Converter::Process::SAMWriter::writeObjectMoveV1(
	Frame::ObjectMove &move, const Frame::ObjectMove &prev)
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

	if (move.matrix.sx != 65536 || move.matrix.sy != 65536 ||
		move.matrix.r0 != 0 || move.matrix.r1 != 0)
	{
		depthAndFlags |= MOVEFLAGS_MATRIX;
	}

	int scaledX = owner.scale(move.matrix.tx, CEIL);
	int scaledY = owner.scale(move.matrix.ty, CEIL);

	if (scaledX > 32767 || scaledX < -32768 || scaledY > 32767 ||
		scaledY < -32768)
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

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::writeObjectMoveV2(
	Frame::ObjectMove &move, const Frame::ObjectMove &prev)
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
				scaledX = owner.scale(move.matrix.tx, CEIL);
				scaledY = owner.scale(move.matrix.ty, CEIL);
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

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::writeFrameLabel(const Frame &frame)
{
	auto &labelName = frame.labelName;

	if (labelName.isEmpty())
		return true;

	return writeString(labelName);
}

bool Converter::Process::SAMWriter::writeFrameFlags(const Frame &frame)
{
	quint8 flags = 0;

	if (not removes.empty())
		flags |= FRAMEFLAGS_REMOVES;

	if (not adds.empty())
		flags |= FRAMEFLAGS_ADDS;

	if (not moves.empty())
		flags |= FRAMEFLAGS_MOVES;

	if (not frame.labelName.isEmpty())
		flags |= FRAMEFLAGS_LABEL;

	stream << flags;

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::writeFrameCount()
{
	stream << quint16(owner.swf.frameCount);

	return outputStreamOk();
}

bool Converter::Process::SAMWriter::outputStreamOk()
{
	if (stream.status() == QDataStream::Ok)
		return true;

	owner.result = OUTPUT_FILE_WRITE_ERROR;
	return false;
}

Converter::Process::Process(Converter *owner)
	: owner(owner)
	, currentFrame(nullptr)
	, jpegTables(nullptr)
	, result(OK)
	, firstDepth(65535)
	, depthMultiplier(0)
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

	prefix = owner->outputFilePath(QFileInfo(owner->mInputFilePath).baseName());

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
