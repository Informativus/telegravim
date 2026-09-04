/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/Qt>

class QKeyEvent;

namespace Core::VimKeymap::Bindings {

struct MatchOptions {
	bool allowExtraShift = false;
	bool realMacModifiers = false;
	bool allowMacControlCommandEquivalent = true;
};

[[nodiscard]] Qt::KeyboardModifiers CleanModifiers(not_null<QKeyEvent*> e);
[[nodiscard]] QString PlainText(not_null<QKeyEvent*> e);
[[nodiscard]] QString HintCharacter(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsPlainEnter(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsPlainEscape(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsLineEnd(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsTextYank(not_null<QKeyEvent*> e);
[[nodiscard]] QString NormalizeToken(QString value);
[[nodiscard]] bool KeyIs(
	not_null<QKeyEvent*> e,
	Qt::Key key,
	const QString &latin,
	const QString &cyrillic = QString());
[[nodiscard]] bool Matches(
	const QString &bindings,
	not_null<QKeyEvent*> e,
	MatchOptions options = {});
[[nodiscard]] int MediaNavigationDelta(not_null<QKeyEvent*> e);

} // namespace Core::VimKeymap::Bindings
