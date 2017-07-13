// Part of SWF to SAM animation converter
// Uses Qt Framework from www.qt.io
// Uses libraries from www.github.com/matthiaskramm/swftools
//
// Copyright (c) 2017 Alexandra Cherdantseva

#include <QCoreApplication>
#include <QCommandLineParser>

#include "Converter.h"

int main(int argc, char *argv[])
{
	QCoreApplication::setApplicationVersion(APP_VERSION);
	QCoreApplication::setApplicationName(APP_NAME);

	QCoreApplication a(argc, argv);

	QCommandLineParser parser;
	parser.setApplicationDescription(APP_DESCRIPTION);
	parser.addVersionOption();
	parser.addHelpOption();

	QCommandLineOption inputOption(
		{"i", "input_file"},
		"Input SWF-file path.",
		"swf");

	QCommandLineOption outputOption(
		{"o", "output_dir"},
		"Output directory path to store SAM-file and images.",
		"path");

	QCommandLineOption scaleOption(
		{"s", "scale"}, "Output scale factor.", "value", "1");

	QCommandLineOption configOption(
		{"c", "config"},
		"Converter configuration JSON-file.\n"
		"==================================\n"
		"Supported properties: {\n"
		"   \"rename_labels\": {\n"
		"     \"<label_name>\": [\n"
		"       \"<old_name1>\", ..., \"<old_nameN>\"\n"
		"     ]\n"
		"   }\n"
		"} ",
		"json");

	parser.addOption(inputOption);
	parser.addOption(outputOption);
	parser.addOption(scaleOption);
	parser.addOption(configOption);

	parser.process(a);

	Converter cvt;

	cvt.setInputFilePath(parser.value(inputOption));
	cvt.setOutputDirPath(parser.value(outputOption));
	cvt.setScale(parser.value(scaleOption).toDouble());
	cvt.loadConfig(parser.value(configOption));

	int result = cvt.exec();

	return result;
}
