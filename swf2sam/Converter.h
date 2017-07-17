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
		UNSUPPORTED_SHAPE,
		UNSUPPORTED_OBJECT_FLAGS,
		UNSUPPORTED_OBJECT_DEPTH,
		UNSUPPORTED_SHAPE_COUNT,
		UNSUPPORTED_DISPLAY_COUNT,
		UNSUPPORTED_ADD_COLOR,
		UNSUPPORTED_TAG,
		UNKNOWN_IMAGE_ID,
		UNKNOWN_SHAPE_ID,
		OUTPUT_DIR_ERROR,
		OUTPUT_FILE_WRITE_ERROR,
		CONFIG_OPEN_ERROR,
		CONFIG_PARSE_ERROR,
		BAD_SCALE_VALUE
	};

	Converter();

	using LabelRenameMap = std::map<QString, QString>;

	void setSkipUnsupported(bool skip);
	void setScale(qreal value);
	void setLabelRenameMap(const LabelRenameMap &value);
	void setInputFilePath(const QString &path);
	void setOutputDirPath(const QString &path);

	void loadConfig(const QString &configFilePath);
	void loadConfigJson(const QByteArray &json);

	int exec();

	QString errorMessage() const;

private:
	struct Process;
	friend struct Process;

	QString outputFilePath(const QString &fileName) const;

	enum
	{
		FLOOR,
		CEIL
	};

	int scale(int value, int mode) const;

	QVariant mErrorInfo;
	QString mInputFilePath;
	QString mOutputDirPath;
	LabelRenameMap mLabelRenameMap;
	qreal mScale;
	bool mSkipUnsupported;
	int mResult;
};

inline void Converter::setSkipUnsupported(bool skip)
{
	mSkipUnsupported = skip;
}

inline void Converter::setScale(qreal value)
{
	mScale = value;
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
