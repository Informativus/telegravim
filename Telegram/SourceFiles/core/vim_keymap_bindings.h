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

namespace Core::VimKeymap {
enum class TextMotion;
} // namespace Core::VimKeymap

namespace Core::VimKeymap::Bindings {

struct MatchOptions {
	bool allowExtraShift = false;
	bool realMacModifiers = false;
	bool allowMacControlCommandEquivalent = true;
};

enum class StickerGridAction {
	None,
	MoveLeft,
	MoveDown,
	MoveUp,
	MoveRight,
	ScrollUp,
	ScrollDown,
	Choose,
	Preview,
	ClosePreview,
};

enum class PaneNavigationAction {
	None,
	Prefix,
	Cancel,
	Left,
	Right,
};

[[nodiscard]] PaneNavigationAction PaneNavigationKey(
	not_null<QKeyEvent*> e,
	bool &pending);

[[nodiscard]] Qt::KeyboardModifiers CleanModifiers(not_null<QKeyEvent*> e);
[[nodiscard]] QString PlainText(not_null<QKeyEvent*> e);
[[nodiscard]] QString HintCharacter(not_null<QKeyEvent*> e);
[[nodiscard]] bool ValidBindings(const QString &bindings);
[[nodiscard]] QString ShortestHintLabel(
	int index,
	int total,
	const QString &alphabet);
[[nodiscard]] bool IsPlainEnter(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsPlainEscape(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsSystemPaste(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsLineStart(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsLineEnd(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsTextYank(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsTextFollowLink(not_null<QKeyEvent*> e, bool pendingStart);
[[nodiscard]] std::optional<TextMotion> TextMotionKey(
	not_null<QKeyEvent*> e,
	bool &pendingStart);
[[nodiscard]] bool IsMessageSelection(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsMessageReaction(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsCloseHints(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsShowMessageHints(not_null<QKeyEvent*> e);
[[nodiscard]] bool IsSelectionForward(not_null<QKeyEvent*> e);
[[nodiscard]] std::optional<float64> MediaPlaybackSpeed(
	not_null<QKeyEvent*> e,
	float64 current);
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
[[nodiscard]] int PageNavigationDelta(not_null<QKeyEvent*> e);
[[nodiscard]] int TabNavigationDelta(not_null<QKeyEvent*> e);
[[nodiscard]] int PickerNavigationDelta(not_null<QKeyEvent*> e);
[[nodiscard]] int MediaNavigationDelta(not_null<QKeyEvent*> e);
[[nodiscard]] int InterfaceHistoryDelta(not_null<QKeyEvent*> e);
[[nodiscard]] bool SpellcheckKey(not_null<QKeyEvent*> e);
[[nodiscard]] StickerGridAction StickerGridActionKey(
	not_null<QKeyEvent*> e);

} // namespace Core::VimKeymap::Bindings
