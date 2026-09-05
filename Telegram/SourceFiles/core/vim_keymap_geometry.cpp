/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/vim_keymap_geometry.h"

#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>

#include <algorithm>
#include <limits>

namespace Core::VimKeymap {

QRect LinkHintTargetRect(
		QPoint hit,
		QRect bounds,
		Fn<bool(QPoint)> matches) {
	if (!bounds.contains(hit) || !matches(hit)) {
		return {};
	}
	auto left = hit.x();
	auto right = left;
	auto top = hit.y();
	auto bottom = top;
	while (left > bounds.left() && matches({ left - 1, hit.y() })) {
		--left;
	}
	while (right < bounds.right() && matches({ right + 1, hit.y() })) {
		++right;
	}
	const auto rowMatches = [&](int y) {
		return matches({ left, y }) && matches({ right, y });
	};
	while (top > bounds.top() && rowMatches(top - 1)) {
		--top;
	}
	while (bottom < bounds.bottom() && rowMatches(bottom + 1)) {
		++bottom;
	}
	return QRect(QPoint(left, top), QPoint(right, bottom));
}

std::vector<QRect> LayoutHintBadges(
		const std::vector<QRect> &desired,
		QRect bounds,
		int gap,
		const std::vector<QRect> &targets) {
	Expects(targets.empty() || targets.size() == desired.size());
	auto result = std::vector<QRect>();
	result.reserve(desired.size());
	gap = std::max(gap, 0);
	for (auto rect : desired) {
		if (rect.isEmpty() || bounds.isEmpty()
			|| rect.width() > bounds.width()
			|| rect.height() > bounds.height()) {
			result.emplace_back();
			continue;
		}
		auto minX = bounds.left();
		auto minY = bounds.top();
		auto maxX = bounds.right() - rect.width() + 1;
		auto maxY = bounds.bottom() - rect.height() + 1;
		if (!targets.empty() && !targets[result.size()].isNull()) {
			const auto target = targets[result.size()].intersected(bounds);
			if (target.isEmpty()) {
				result.emplace_back();
				continue;
			}
			const auto center = QRect(QPoint(), rect.size()).center();
			minX = std::max(minX, target.left() - center.x());
			maxX = std::min(maxX, target.right() - center.x());
			minY = std::max(minY, target.top() - center.y());
			maxY = std::min(maxY, target.bottom() - center.y());
		}
		if (minX > maxX || minY > maxY) {
			result.emplace_back();
			continue;
		}
		rect.moveLeft(std::clamp(rect.x(), minX, maxX));
		rect.moveTop(std::clamp(rect.y(), minY, maxY));
		const auto free = [&](QRect candidate) {
			return std::ranges::none_of(result, [&](QRect placed) {
				return !placed.isEmpty()
					&& placed.marginsAdded(QMargins(gap, gap, gap, gap))
						.intersects(candidate);
			});
		};
		if (free(rect)) {
			result.push_back(rect);
			continue;
		}
		auto rows = std::vector<int>{ rect.y(), minY, maxY };
		for (const auto placed : result) {
			if (!placed.isEmpty()) {
				rows.push_back(std::clamp(
					placed.top() - gap - rect.height(), minY, maxY));
				rows.push_back(std::clamp(
					placed.bottom() + gap + 1, minY, maxY));
			}
		}
		std::ranges::sort(rows);
		rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
		auto best = QRect();
		auto distance = std::numeric_limits<int>::max();
		for (const auto y : rows) {
			if (std::abs(y - rect.y()) > distance) {
				continue;
			}
			auto blocked = std::vector<std::pair<int, int>>();
			for (const auto placed : result) {
				if (!placed.isEmpty()
					&& y <= placed.bottom() + gap
					&& y + rect.height() > placed.top() - gap) {
					blocked.emplace_back(
						placed.left() - gap - rect.width() + 1,
						placed.right() + gap);
				}
			}
			std::ranges::sort(blocked);
			const auto consider = [&](int left, int right) {
				if (left > right) {
					return;
				}
				const auto x = std::clamp(rect.x(), left, right);
				const auto delta = std::abs(x - rect.x()) + std::abs(y - rect.y());
				if (delta < distance) {
					distance = delta;
					best = QRect(QPoint(x, y), rect.size());
				}
			};
			auto left = minX;
			for (const auto &[from, till] : blocked) {
				consider(left, std::min(maxX, from - 1));
				left = std::max(left, till + 1);
			}
			consider(left, maxX);
		}
		result.push_back(best);
	}
	return result;
}

void PaintHintBadges(
		QPainter &p,
		const std::vector<HintBadge> &hints,
		const QString &prefix,
		const QFont &font,
		QRect bounds,
		QSize padding,
		int gap,
		bool visual) {
	const auto metrics = QFontMetrics(font);
	auto labels = std::vector<QString>();
	auto desired = std::vector<QRect>();
	auto targets = std::vector<QRect>();
	for (const auto &hint : hints) {
		if (!hint.label.startsWith(prefix)) {
			continue;
		}
		const auto remaining = hint.label.mid(prefix.size());
		labels.push_back(remaining.isEmpty() ? hint.label : remaining);
		targets.push_back(hint.target);
		desired.emplace_back(hint.anchor, QSize(
			metrics.horizontalAdvance(labels.back()) + 2 * padding.width(),
			metrics.height() + 2 * padding.height()));
	}
	const auto rects = LayoutHintBadges(desired, bounds, gap, targets);
	p.save();
	p.setClipRect(bounds, Qt::IntersectClip);
	p.setFont(font);
	p.setRenderHint(QPainter::Antialiasing, true);
	for (auto i = 0; i != rects.size(); ++i) {
		const auto rect = rects[i];
		if (rect.isEmpty()) {
			continue;
		}
		const auto radius = rect.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(visual
			? QColor(218, 91, 166, 246)
			: QColor(255, 218, 72, 246));
		p.drawRoundedRect(QRectF(rect), radius, radius);
		p.setPen(visual ? QColor(255, 255, 255) : QColor(28, 24, 14));
		p.drawText(rect, Qt::AlignCenter, labels[i]);
	}
	p.restore();
}

void PaintMessageCursor(QPainter &p, QRect characterRect) {
	if (characterRect.isEmpty()) {
		return;
	}
	p.save();
	p.setCompositionMode(QPainter::CompositionMode_Difference);
	p.fillRect(characterRect, Qt::white);
	p.restore();
}

QRect CursorPaintRect(
		QRect characterRect,
		const QString &style,
		int requestedWidth,
		int requestedHeight) {
	const auto lineHeight = std::max(1, characterRect.height());
	const auto characterWidth = std::max(
		std::max(1, characterRect.width()),
		requestedWidth);
	const auto height = std::clamp(requestedHeight, 1, lineHeight);
	if (style == u"underline"_q) {
		const auto thickness = std::clamp(requestedWidth, 1, lineHeight);
		return QRect(
			characterRect.left(),
			characterRect.bottom() - thickness + 1,
			characterWidth,
			thickness);
	}
	return QRect(
		characterRect.left(),
		characterRect.top() + (lineHeight - height) / 2,
		(style == u"block"_q) ? characterWidth : requestedWidth,
		height);
}

VisualSelectionRange MakeVisualSelectionRange(
		int anchor,
		int focus,
		int textLength) {
	if (textLength <= 0) {
		return {};
	}
	anchor = std::clamp(anchor, 0, textLength - 1);
	focus = std::clamp(focus, 0, textLength - 1);
	return {
		.from = std::min(anchor, focus),
		.till = std::max(anchor, focus) + 1,
	};
}

bool TextVisualModeConsumesKey(bool visualMode, bool visualKey) {
	return visualMode && visualKey;
}

bool TextVisualYankCompletes(bool selectionActive, bool copied) {
	return selectionActive && copied;
}

bool EmptyComposeDefersToMessageAction(
		bool composeStateActive,
		bool fieldEmpty,
		bool messageAction) {
	return !composeStateActive && fieldEmpty && messageAction;
}

bool ShouldAddScannedLinkHint(
		bool serviceMessage,
		bool hasLink,
		bool overMessageText) {
	return hasLink && (serviceMessage || overMessageText);
}

bool ShouldAddInlinePlaybackHint(
		bool voiceOrVideoMessage,
		bool mediaVisible,
		bool scannedPlaybackLink,
		bool directPlaybackLink) {
	return voiceOrVideoMessage
		&& mediaVisible
		&& !scannedPlaybackLink
		&& directPlaybackLink;
}

bool ShouldAddStickerHint(
		bool sticker,
		bool mediaVisible,
		bool hasLink) {
	return sticker && mediaVisible && hasLink;
}

int MoveStickerGridSelection(int selected, int count, int delta) {
	if (count <= 0 || !delta) {
		return -1;
	} else if (selected < 0 || selected >= count) {
		return 0;
	}
	return std::clamp(selected + delta, 0, count - 1);
}

QRect GroupedMediaHintRect(
		QRect groupItemRect,
		QPoint itemInnerTopLeft,
		int itemTop) {
	return groupItemRect.translated(
		itemInnerTopLeft + QPoint(0, itemTop));
}

int ResolveTextCursorOffset(
		int wanted,
		int direction,
		int textLength,
		Fn<bool(int)> isSelectable) {
	if (textLength <= 0) {
		return -1;
	}
	wanted = std::clamp(wanted, 0, textLength - 1);
	direction = (direction < 0) ? -1 : 1;
	if (isSelectable(wanted)) {
		return wanted;
	}
	for (auto offset = wanted + direction;
			offset >= 0 && offset < textLength;
			offset += direction) {
		if (isSelectable(offset)) {
			return offset;
		}
	}
	for (auto offset = wanted - direction;
			offset >= 0 && offset < textLength;
			offset -= direction) {
		if (isSelectable(offset)) {
			return offset;
		}
	}
	return -1;
}

} // namespace Core::VimKeymap
