/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "iv/iv_rich_page.h"

#include <QtCore/QJsonObject>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

class DocumentData;
class PhotoData;

namespace Data {

struct LocalQuickReplyRichMedia final {
	std::vector<PhotoData*> photos;
	std::vector<DocumentData*> documents;
};

[[nodiscard]] std::variant<QJsonObject, QString> EncodeLocalQuickReplyRich(
	const Iv::RichPage &page,
	Fn<std::optional<int>(PhotoData*)> photoAsset,
	Fn<std::optional<int>(DocumentData*)> documentAsset);

[[nodiscard]] QString ValidateLocalQuickReplyRich(
	const QJsonObject &object);

[[nodiscard]] QString VisitLocalQuickReplyRichText(
	const QJsonObject &object,
	Fn<bool(const TextWithEntities &)> visit,
	int *fixedTextLength = nullptr);

[[nodiscard]] std::variant<QJsonObject, QString> TransformLocalQuickReplyRichText(
	const QJsonObject &object,
	Fn<std::optional<TextWithEntities>(const TextWithEntities &)> transform);

[[nodiscard]] std::variant<std::shared_ptr<Iv::RichPage>, QString>
DecodeLocalQuickReplyRich(
	const QJsonObject &object,
	Fn<PhotoData*(int)> photoAsset,
	Fn<DocumentData*(int)> documentAsset);

[[nodiscard]] std::variant<LocalQuickReplyRichMedia, QString>
CollectLocalQuickReplyRichMedia(const Iv::RichPage &page);

} // namespace Data
