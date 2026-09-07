/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/spellchecker_bundled.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

namespace Spellchecker {
namespace {

[[nodiscard]] bool WriteResource(
		const QString &name,
		const QString &destination) {
	const auto parts = (name == u"ru_RU.dic"_q)
		? QStringList{ name + u".1"_q, name + u".2"_q }
		: QStringList{ name };
	auto bytes = QByteArray();
	for (const auto &part : parts) {
		auto resource = QFile(u":/dictionaries/"_q + part);
		if (!resource.open(QIODevice::ReadOnly)) {
			return false;
		}
		const auto content = resource.readAll();
		if (content.isEmpty()) {
			return false;
		}
		bytes += content;
	}
	auto existing = QFile(destination);
	if (existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes) {
		return true;
	}
	existing.close();
	auto output = QSaveFile(destination);
	return output.open(QIODevice::WriteOnly)
		&& output.write(bytes) == bytes.size()
		&& output.commit();
}

} // namespace

std::vector<int> InstallBundledDictionaries(const QString &workingDir) {
	auto installed = std::vector<int>();
	for (const auto language : { QLocale::Russian, QLocale::English }) {
		const auto name = QLocale(language).name();
		const auto folder = QDir(workingDir).filePath(name);
		if (!QDir().mkpath(folder)) {
			continue;
		}
		auto complete = true;
		for (const auto &suffix : { u".aff"_q, u".dic"_q }) {
			const auto file = name + suffix;
			if (!WriteResource(file, QDir(folder).filePath(file))) {
				complete = false;
				break;
			}
		}
		if (complete) {
			installed.push_back(int(language));
		}
	}
	return installed;
}

} // namespace Spellchecker
