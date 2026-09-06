/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QStringList>

class QKeyEvent;

namespace Core::VimKeymap {

class KeyEventLog final {
public:
	static constexpr auto kLimit = 200;

	void record(not_null<QKeyEvent*> event, const QString &status);
	void clear();
	[[nodiscard]] QString text() const;
	[[nodiscard]] uint64 generation() const;
	[[nodiscard]] bool suppressed() const;
	void setSuppressed(bool suppressed);

private:
	QStringList _entries;
	uint64 _generation = 0;
	uint64 _sequence = 0;
	bool _suppressed = false;

};

} // namespace Core::VimKeymap
