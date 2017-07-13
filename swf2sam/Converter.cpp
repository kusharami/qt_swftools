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

		auto &oldNames = renameMap[it.key()];

		if (value.isString())
		{
			oldNames.append(value.toString());
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

				oldNames.append(av.toString());
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

	Image(TAG *tag, TAG *jpegTables, size_t index);

	int id() const;

	bool exportImage(const QString &prefix, QString *imageFileName);
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

	QString labelName;

	std::vector<quint16> removes;
	std::vector<ObjectAdd> adds;
	std::vector<ObjectMove> moves;
};

static inline quint8 cxToByte(S16 cx)
{
	return quint8((cx / WORD_TO_FLOAT) * 255.0);
}

int Converter::exec()
{
	mResult = OK;

	if (mScale < 0.1)
	{
		mResult = BAD_SCALE_VALUE;
		return mResult;
	}

	bool ok;
	SWF swf;
	{
		QFile inputFile(mInputFilePath);

		if (not inputFile.open(QFile::ReadOnly))
		{
			mResult = INPUT_FILE_OPEN_ERROR;
			return mResult;
		}

		if (not QDir().mkpath(mOutputDirPath))
		{
			mResult = OUTPUT_DIR_ERROR;
			return mResult;
		}

		reader_t reader;
		QIODeviceSWFReader::init(&reader, &inputFile);

		ok = swf_ReadSWF2(&reader, &swf) >= 0;

		reader.dealloc(&reader);

		if (not ok)
		{
			mResult = INPUT_FILE_FORMAT_ERROR;
			return mResult;
		}
	}

	std::vector<Image> images;
	std::vector<Shape> shapes;
	std::vector<Frame> frames(swf.frameCount);
	TAG *jpegTables = nullptr;

	std::map<int, size_t> imageMap;
	std::map<int, size_t> shapeMap;

	Frame *currentFrame = &frames.front();

	auto tag = swf.firstTag;
	while (tag && ok)
	{
		switch (tag->id)
		{
			case ST_FILEATTRIBUTES:
			case ST_SETBACKGROUNDCOLOR:
			case ST_SCENEDESCRIPTION:
				// ignore
				break;

			case ST_SHOWFRAME:
				if (currentFrame == nullptr)
				{
					ok = false;
					mResult = INPUT_FILE_BAD_DATA_ERROR;
					break;
				}

				currentFrame++;
				if (currentFrame > &frames.back())
				{
					currentFrame = nullptr;
				}
				break;

			case ST_FRAMELABEL:
			{
				if (currentFrame == nullptr)
				{
					ok = false;
					mResult = INPUT_FILE_BAD_DATA_ERROR;
					break;
				}

				auto str = reinterpret_cast<char *>(tag->data);
				currentFrame->labelName =
					swf.fileVersion >= 6
					? QString::fromUtf8(str)
					: QString::fromLocal8Bit(str);
				break;
			}

			case ST_PLACEOBJECT:
			case ST_PLACEOBJECT2:
			case ST_PLACEOBJECT3:
			{
				if (currentFrame == nullptr)
				{
					ok = false;
					mResult = INPUT_FILE_BAD_DATA_ERROR;
					break;
				}

				SWFPLACEOBJECT srcObj;
				swf_GetPlaceObject(tag, &srcObj);

				if (srcObj.flags & ~(PF_CHAR | PF_CXFORM |
									 PF_MATRIX | PF_MOVE | PF_NAME))
				{
					ok = false;
					mErrorInfo = srcObj.flags;
					mResult = UNSUPPORTED_SWF_OBJECT_FLAGS;
					break;
				}

				if (srcObj.depth > 0x3FF)
				{
					ok = false;
					mErrorInfo = srcObj.depth;
					mResult = UNSUPPORTED_SWF_OBJECT_DEPTH;
					break;
				}

				quint16 depth = srcObj.depth;

				if (srcObj.flags & PF_CHAR)
				{
					auto shapeIt = shapeMap.find(srcObj.id);
					if (shapeIt == shapeMap.end() || shapeIt->second > 255)
					{
						ok = false;
						mErrorInfo = srcObj.id;
						mResult = UNKNOWN_SWF_SHAPE_ID;
						break;
					}

					Frame::ObjectAdd add;
					add.depth = depth;
					add.shapeId = quint8(shapeIt->second);

					currentFrame->adds.push_back(add);
				}

				Frame::ObjectMove move;
				move.depthAndFlags = 0;

				if (srcObj.flags & PF_CXFORM)
				{
					if (srcObj.cxform.a1 != 0 ||
						srcObj.cxform.r1 != 0 ||
						srcObj.cxform.g1 != 0 ||
						srcObj.cxform.b1 != 0)
					{
						ok = false;
						mErrorInfo = srcObj.id;
						mResult = UNSUPPORTED_SWF_ADD_COLOR;
						break;
					}
					move.depthAndFlags |= MOVEFLAGS_COLOR;
				}

				if (srcObj.flags & PF_MATRIX)
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

					currentFrame->moves.push_back(move);
				}
				break;
			}

			case ST_REMOVEOBJECT:
			case ST_REMOVEOBJECT2:
				if (currentFrame == nullptr)
				{
					ok = false;
					mResult = INPUT_FILE_BAD_DATA_ERROR;
					break;
				}

				currentFrame->removes.push_back(quint16(swf_GetDepth(tag)));
				break;

			case ST_JPEGTABLES:
				jpegTables = tag;
				break;

			case ST_DEFINEBITSLOSSLESS:
			case ST_DEFINEBITSLOSSLESS2:
			case ST_DEFINEBITSJPEG:
			case ST_DEFINEBITSJPEG2:
			case ST_DEFINEBITSJPEG3:
			{
				auto index = images.size();
				images.push_back(Image(tag, jpegTables, index));
				imageMap[GET16(tag->data)] = index;
				break;
			}

			case ST_DEFINESHAPE:
			case ST_DEFINESHAPE2:
			case ST_DEFINESHAPE3:
			case ST_DEFINESHAPE4:
			{
				SHAPE2 srcShape;

				swf_ParseDefineShape(tag, &srcShape);
				int shapeId = GET16(tag->data);
				mErrorInfo = shapeId;

				if (srcShape.numlinestyles > 0)
				{
					ok = false;
					mResult = UNSUPPORTED_SWF_LINESTYLES;
					break;
				}

				Image *img = nullptr;

				auto index = shapes.size();
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
								ok = false;
								mErrorInfo = imageId;
								mResult = UNKNOWN_SWF_IMAGE_ID;
								break;
							}

							if (nullptr != img)
							{
								ok = false;
								mResult = UNSUPPORTED_SWF_SHAPE;
								break;
							}

							shape.imageIndex = it->second;
							img = &images.at(shape.imageIndex);
							Q_ASSERT(nullptr != img);

							shape.matrix = fillStyle.m;
							break;
						}

						default:
							ok = false;
							mErrorInfo = fillStyle.type;
							mResult = UNSUPPORTED_SWF_FILLSTYLE;
							break;
					}
				}

				if (nullptr == img)
				{
					ok = false;
					mResult = UNSUPPORTED_SWF_SHAPE;
					break;
				}

				auto line = srcShape.lines;

				int minX = 0;
				int minY = 0;
				int maxX = 0;
				int maxY = 0;
				int curX = 0, curY = 0;
				int lineCount = 0;
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
						mResult = UNSUPPORTED_SWF_SHAPE;
					}
				}

				if (ok)
				{
					if (minX != shape.matrix.tx || minY != shape.matrix.ty)
					{
						ok = false;
						mResult = UNSUPPORTED_SWF_SHAPE;
						break;
					}

					shape.width = maxX - minX;
					shape.height = maxY - minY;
				}
				break;
			}

			default:
				ok = false;
				mErrorInfo = tag->id;
				mResult = UNSUPPORTED_SWF_TAG;
				break;
		}

		tag = swf_NextTag(tag);
	}

	if (not ok)
	{
		return mResult;
	}

	auto prefix = outputFilePath(QFileInfo(mInputFilePath).baseName());
	QSaveFile samFile(prefix + ".sam");

	if (not samFile.open(QFile::WriteOnly | QFile::Truncate))
	{
		mErrorInfo = samFile.fileName();
		mResult = OUTPUT_FILE_WRITE_ERROR;
		return mResult;
	}

	do
	{
		QDataStream samStream(&samFile);
		samStream.setByteOrder(QDataStream::LittleEndian);

		SAM_Header header;
		memcpy(header.signature, SAM_Signature, SAM_SIGN_SIZE);
		header.version = SAM_VERSION;
		header.frame_rate = quint8(swf.frameRate >> 8);
		header.x = scale(swf.movieSize.xmin, FLOOR);
		header.y = scale(swf.movieSize.ymin, FLOOR);
		header.width = scale(swf.movieSize.xmax, CEIL) - header.x;
		header.height = scale(swf.movieSize.ymax, CEIL) - header.y;

		samStream.writeRawData(header.signature, SAM_SIGN_SIZE);
		samStream << header.version;
		samStream << header.frame_rate;
		samStream << header.x;
		samStream << header.y;
		samStream << header.width;
		samStream << header.height;

		samStream << swf.frameCount;

		if (samStream.status() != QDataStream::Ok)
		{
			ok = false;
			break;
		}
	} while (false);

	if (not ok || not samFile.commit())
	{
		mErrorInfo = samFile.fileName();
		mResult = OUTPUT_FILE_WRITE_ERROR;
	}

	return mResult;
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

bool Image::exportImage(const QString &prefix, QString *imageFileName)
{
	static const QString nameFmt("%2_%3.%1");

	QString imageFilePath;
	switch (tag->id)
	{
		case ST_DEFINEBITSLOSSLESS:
		case ST_DEFINEBITSLOSSLESS2:
		case ST_DEFINEBITSJPEG3:
			imageFilePath = nameFmt.arg("png");
			break;

		case ST_DEFINEBITSJPEG:
		case ST_DEFINEBITSJPEG2:
			imageFilePath = nameFmt.arg("jpg");
			break;

		default:
			return false;
	}

	imageFilePath = imageFilePath.arg(prefix).arg(index + 1, 4, 10, QChar('0'));

	if (not QDir().mkpath(QFileInfo(prefix).path()))
	{
		return false;
	}

	QSaveFile file(imageFilePath);

	if (not file.open(QFile::WriteOnly | QFile::Truncate))
	{
		return false;
	}

	QFileInfo fileInfo(imageFilePath);
	if (nullptr != imageFileName)
	{
		*imageFileName = fileInfo.fileName();
	}

	int writeLen;
	int tagEnd = tag->len;

	QImage image;

	switch (tag->id)
	{
		case ST_DEFINEBITSJPEG:
		case ST_DEFINEBITSJPEG2:
		{
			int skip = 2;
			switch (tag->id)
			{
				case ST_DEFINEBITSJPEG:
				{
					if (nullptr != jpegTables && jpegTables->len >= 2)
					{
						writeLen = jpegTables->len - 2;
						skip += 2;
						if (file.write(
								reinterpret_cast<char *>(jpegTables->data),
								writeLen) != writeLen)
						{
							return false;
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

						if (file.write(
								reinterpret_cast<char *>(&tag->data[2]),
								writeLen) != writeLen)
						{
							return false;
						}

						skip += pos + 4;
					}
					break;
				}
			}

			writeLen = tagEnd - skip;
			if (writeLen > 0 && file.write(
					reinterpret_cast<char *>(&tag->data[skip]),
					writeLen) != writeLen)
			{
				return false;
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
					return false;

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
						return false;
					}

					if (uncompressedSize != uLong(alphaSize))
						return false;

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
					return false;
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

	if (not image.isNull() && not image.save(&file, "png"))
	{
		return false;
	}
	return file.commit();
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
