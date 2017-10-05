// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses source code from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#pragma once

#include <QString>
#include <QVariant>

#include <map>

class Converter
{
public:
	enum
	{
		OK,
		INPUT_FILE_OPEN_ERROR,
		INPUT_FILE_FORMAT_ERROR,
		INPUT_FILE_BAD_DATA_ERROR,
		UNSUPPORTED_LINESTYLES,
		UNSUPPORTED_FILLSTYLE,
		UNSUPPORTED_VECTOR_SHAPE,
		UNSUPPORTED_NOBITMAP_SHAPE,
		UNSUPPORTED_OBJECT_FLAGS,
		UNSUPPORTED_OBJECT_DEPTH,
		UNSUPPORTED_SHAPE_COUNT,
		UNSUPPORTED_DISPLAY_COUNT,
		UNSUPPORTED_TAG,
		UNKNOWN_IMAGE_ID,
		UNKNOWN_SHAPE_ID,
		OUTPUT_DIR_ERROR,
		OUTPUT_FILE_WRITE_ERROR,
		CONFIG_OPEN_ERROR,
		CONFIG_PARSE_ERROR,
		BAD_SCALE_VALUE,
		BAD_SAM_VERSION
	};

	Converter();

	using LabelRenameMap = std::map<QString, QString>;

	void setSkipUnsupported(bool skip);
	void setScale(qreal value);
	void setSamVersion(int value);
	void setLabelRenameMap(const LabelRenameMap &value);
	void setInputFilePath(const QString &path);
	void setOutputDirPath(const QString &path);

	void loadConfig(const QString &configFilePath);
	void loadConfigJson(const QByteArray &json);

	int exec();
	inline int result() const;
	inline const QVariant &errorInfo() const;
	static QString tagName(const QVariant &t);
	static QString tagName(quint16 t);
	static QString fillStyleToStr(int value);

	QString errorMessage() const;

	struct Warning
	{
		int code;
		QVariant info;
	};

	using Warnings = std::vector<Warning>;

	inline const Warnings &warnings() const;

private:
	static QString warnMessage(const Warning &warn);

	struct Process;
	friend struct Process;

	QString outputFilePath(const QString &fileName) const;

	enum
	{
		FLOOR,
		CEIL
	};

	int scale(int value, int mode) const;

	Warnings mWarnings;
	QVariant mErrorInfo;
	QString mInputFilePath;
	QString mOutputDirPath;
	LabelRenameMap mLabelRenameMap;
	qreal mScale;
	int mSamVersion;
	int mResult;
	bool mSkipUnsupported;
};

inline void Converter::setSkipUnsupported(bool skip)
{
	mSkipUnsupported = skip;
}

inline void Converter::setScale(qreal value)
{
	mScale = value;
}

inline void Converter::setSamVersion(int value)
{
	mSamVersion = value;
}

inline void Converter::setLabelRenameMap(const LabelRenameMap &value)
{
	mLabelRenameMap = value;
}

inline void Converter::setInputFilePath(const QString &path)
{
	mInputFilePath = path;
}

inline void Converter::setOutputDirPath(const QString &path)
{
	mOutputDirPath = path;
}

int Converter::result() const
{
	return mResult;
}

const QVariant &Converter::errorInfo() const
{
	return mErrorInfo;
}

const Converter::Warnings &Converter::warnings() const
{
	return mWarnings;
}
