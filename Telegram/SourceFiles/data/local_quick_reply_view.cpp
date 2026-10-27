/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/local_quick_reply_view.h"

#include "base/random.h"
#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_game.h"
#include "data/data_media_types.h"
#include "data/data_messages.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_web_page.h"
#include "data/local_quick_reply_media.h"
#include "data/local_quick_reply_rich_codec.h"
#include "history/history.h"
#include "history/history_item_edition.h"
#include "main/main_session.h"
#include "ui/image/image_location_factory.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>
#include <crl/crl_queue.h>

#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QUuid>
#include <QtGui/QImageReader>

#include <cstring>

namespace Data {
namespace {

constexpr auto kPreviewMemoryLimit = int64(128) * 1024 * 1024;

[[nodiscard]] crl::queue &PreviewQueue() {
	static auto result = crl::queue();
	return result;
}

[[nodiscard]] QByteArray Revision(const LocalQuickReplyMessage &message) {
	auto entities = QJsonArray();
	for (const auto &entity : message.message.entities) {
		entities.push_back(QJsonObject{
			{ u"type"_q, int(entity.type()) },
			{ u"offset"_q, entity.offset() },
			{ u"length"_q, entity.length() },
			{ u"data"_q, entity.data() },
		});
	}
	return QJsonDocument(QJsonObject{
		{ u"retained"_q, message.retained },
		{ u"entities"_q, entities },
		{ u"reply_ref"_q, message.replyRef },
		{ u"group_ref"_q, message.groupRef },
		{ u"reply_to_ref"_q, message.replyToRef },
		{ u"position"_q, message.position },
		{ u"text"_q, message.message.text },
		{ u"attachments"_q, message.attachments },
		{ u"source_paths"_q, QJsonArray::fromStringList(message.sourcePaths) },
		{ u"media"_q, message.media },
	}).toJson(QJsonDocument::Compact);
}

[[nodiscard]] int AssetIndex(const QJsonObject &media, const QString &role) {
	const auto value = media.value(role);
	return value.isDouble() && value.toDouble() >= 0
		&& value.toDouble() == value.toInt(-1) ? value.toInt() : -1;
}

[[nodiscard]] bool IsDocument(const QString &kind) {
	return kind == u"file"_q || kind == u"video"_q || kind == u"round_video"_q
		|| kind == u"voice"_q || kind == u"audio"_q || kind == u"sticker"_q
		|| kind == u"animation"_q;
}

[[nodiscard]] MTPInputStickerSet StickerSet(const QJsonObject &media) {
	const auto set = media.value(u"sticker_set"_q).toObject();
	const auto name = set.value(u"short_name"_q).toString();
	if (!name.isEmpty()) return MTP_inputStickerSetShortName(MTP_string(name));
	const auto id = set.value(u"id"_q).toString().toULongLong();
	return id ? MTP_inputStickerSetID(MTP_long(id), MTP_long(0))
		: MTP_inputStickerSetEmpty();
}

[[nodiscard]] QVector<MTPDocumentAttribute> Attributes(
		const QJsonObject &media,
		const QJsonObject &attachment) {
	const auto kind = media.value(u"kind"_q).toString();
	auto result = QVector<MTPDocumentAttribute>{ MTP_documentAttributeFilename(
		MTP_string(attachment.value(u"filename"_q).toString())) };
	const auto width = media.value(u"width"_q).toInt();
	const auto height = media.value(u"height"_q).toInt();
	if (kind == u"video"_q || kind == u"round_video"_q) {
		using Flag = MTPDdocumentAttributeVideo::Flag;
		auto flags = MTPDdocumentAttributeVideo::Flags();
		if (kind == u"round_video"_q) flags |= Flag::f_round_message;
		if (media.value(u"supports_streaming"_q).toBool()) flags |= Flag::f_supports_streaming;
		if (media.value(u"nosound"_q).toBool()) flags |= Flag::f_nosound;
		if (media.contains(u"preload_prefix_size"_q)) flags |= Flag::f_preload_prefix_size;
		if (media.contains(u"video_start_ms"_q)) flags |= Flag::f_video_start_ts;
		if (media.contains(u"video_codec"_q)) flags |= Flag::f_video_codec;
		result.push_back(MTP_documentAttributeVideo(MTP_flags(flags),
			MTP_double(media.value(u"duration"_q).toDouble()),
			MTP_int(width), MTP_int(height),
			MTP_int(media.value(u"preload_prefix_size"_q).toInt()),
			MTP_double(media.value(u"video_start_ms"_q).toDouble() / 1000.),
			MTP_string(media.value(u"video_codec"_q).toString())));
	} else if (kind == u"voice"_q || kind == u"audio"_q) {
		using Flag = MTPDdocumentAttributeAudio::Flag;
		result.push_back(MTP_documentAttributeAudio(
			MTP_flags((kind == u"voice"_q ? Flag::f_voice | Flag::f_waveform
				: Flag::f_title | Flag::f_performer)),
			MTP_int(media.value(u"duration"_q).toInt()),
			MTP_string(media.value(u"title"_q).toString()),
			MTP_string(media.value(u"performer"_q).toString()),
			MTP_bytes(QByteArray::fromBase64(
				media.value(u"waveform"_q).toString().toLatin1()))));
	} else if (kind == u"sticker"_q) {
		if (media.value(u"custom_emoji"_q).toBool()) {
			using Flag = MTPDdocumentAttributeCustomEmoji::Flag;
			result.push_back(MTP_documentAttributeCustomEmoji(MTP_flags(
				(media.value(u"free"_q).toBool() ? Flag::f_free : Flag())
				| (media.value(u"text_color"_q).toBool() ? Flag::f_text_color : Flag())),
				MTP_string(media.value(u"alt"_q).toString()), StickerSet(media)));
		} else {
			using Flag = MTPDdocumentAttributeSticker::Flag;
			const auto mask = media.value(u"mask_coords"_q).toObject();
			result.push_back(MTP_documentAttributeSticker(MTP_flags(
				(media.value(u"mask"_q).toBool() ? Flag::f_mask : Flag())
				| (!mask.isEmpty() ? Flag::f_mask_coords : Flag())),
				MTP_string(media.value(u"alt"_q).toString()), StickerSet(media),
				MTP_maskCoords(MTP_int(mask.value(u"n"_q).toInt()),
					MTP_double(mask.value(u"x"_q).toDouble()),
					MTP_double(mask.value(u"y"_q).toDouble()),
					MTP_double(mask.value(u"zoom"_q).toDouble()))));
		}
	}
	if (width > 0 && height > 0 && kind != u"video"_q && kind != u"round_video"_q) {
		result.push_back(MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)));
	}
	if (kind == u"animation"_q) result.push_back(MTP_documentAttributeAnimated());
	return result;
}

[[nodiscard]] MTPGeoPoint GeoPoint(const QJsonObject &media) {
	const auto point = media.value(u"point"_q).toObject();
	return MTP_geoPoint(MTP_flags(point.contains(u"accuracy_radius"_q)
		? MTPDgeoPoint::Flag::f_accuracy_radius : MTPDgeoPoint::Flag()),
		MTP_double(point.value(u"longitude"_q).toDouble()),
		MTP_double(point.value(u"latitude"_q).toDouble()), MTP_long(0),
		MTP_int(point.value(u"accuracy_radius"_q).toInt()));
}

[[nodiscard]] HistoryMessageMarkupData Markup(
		const QJsonObject &object,
		QString *error) {
	using Type = HistoryMessageMarkupButton::Type;
	const auto types = std::vector<std::pair<QString, Type>>{
		{ u"default"_q, Type::Default }, { u"url"_q, Type::Url },
		{ u"callback"_q, Type::Callback }, { u"callback_password"_q, Type::CallbackWithPassword },
		{ u"request_phone"_q, Type::RequestPhone }, { u"request_location"_q, Type::RequestLocation },
		{ u"request_poll"_q, Type::RequestPoll }, { u"request_peer"_q, Type::RequestPeer },
		{ u"switch_inline"_q, Type::SwitchInline }, { u"switch_inline_same"_q, Type::SwitchInlineSame },
		{ u"game"_q, Type::Game }, { u"buy"_q, Type::Buy }, { u"auth"_q, Type::Auth },
		{ u"user_profile"_q, Type::UserProfile }, { u"web_view"_q, Type::WebView },
		{ u"simple_web_view"_q, Type::SimpleWebView }, { u"copy_text"_q, Type::CopyText },
		{ u"disabled"_q, Type::Disabled }, { u"suggest_decline"_q, Type::SuggestDecline },
		{ u"suggest_accept"_q, Type::SuggestAccept }, { u"suggest_change"_q, Type::SuggestChange },
		{ u"create_bot"_q, Type::CreateBot },
	};
	auto result = HistoryMessageMarkupData();
	if (object.isEmpty()) return result;
	result.flags = {};
	for (const auto &[key, flag] : {
		std::pair(u"inline"_q, ReplyMarkupFlag::Inline),
		std::pair(u"resize"_q, ReplyMarkupFlag::Resize),
		std::pair(u"single_use"_q, ReplyMarkupFlag::SingleUse),
		std::pair(u"selective"_q, ReplyMarkupFlag::Selective),
		std::pair(u"persistent"_q, ReplyMarkupFlag::Persistent),
		std::pair(u"force_reply"_q, ReplyMarkupFlag::ForceReply),
		std::pair(u"hide"_q, ReplyMarkupFlag::None) }) {
		if (object.value(key).toBool()) result.flags |= flag;
	}
	result.placeholder = object.value(u"placeholder"_q).toString();
	for (const auto &row : object.value(u"rows"_q).toArray()) {
		auto buttons = std::vector<HistoryMessageMarkupButton>();
		for (const auto &value : row.toArray()) {
			const auto button = value.toObject();
			const auto name = button.value(u"type"_q).toString();
			const auto i = ranges::find(types, name, &std::pair<QString, Type>::first);
			if (i == types.end()) {
				*error = u"This message has an unsupported keyboard button."_q;
				return {};
			}
			const auto type = i->second;
			const auto color = button.value(u"color"_q).toInt();
			if (color < 0 || color > int(HistoryMessageMarkupButton::Color::Success)) {
				*error = u"This message has an unsupported keyboard style."_q;
				return {};
			}
			auto data = QByteArray::fromBase64(button.value(u"data"_q).toString().toLatin1());
			if (type == Type::RequestPeer) {
				const auto source = button.value(u"request_peer"_q).toObject();
				auto query = RequestPeerQuery();
				query.type = RequestPeerQuery::Type(source.value(u"type"_q).toInt());
				query.maxQuantity = source.value(u"max_quantity"_q).toInt();
				query.userIsBot = RequestPeerQuery::Restriction(source.value(u"user_is_bot"_q).toInt());
				query.userIsPremium = RequestPeerQuery::Restriction(source.value(u"user_is_premium"_q).toInt());
				query.groupIsForum = RequestPeerQuery::Restriction(source.value(u"group_is_forum"_q).toInt());
				query.hasUsername = RequestPeerQuery::Restriction(source.value(u"has_username"_q).toInt());
				query.amCreator = source.value(u"am_creator"_q).toBool();
				query.isBotParticipant = source.value(u"bot_participant"_q).toBool();
				query.myRights = ChatAdminRights::from_raw(ChatAdminRights::Type(source.value(u"my_rights"_q).toDouble()));
				query.botRights = ChatAdminRights::from_raw(ChatAdminRights::Type(source.value(u"bot_rights"_q).toDouble()));
				data = QByteArray(sizeof(query), Qt::Uninitialized);
				std::memcpy(data.data(), &query, sizeof(query));
			} else if (type == Type::CreateBot) {
				auto stream = QDataStream(&data, QIODevice::WriteOnly);
				stream << button.value(u"suggested_name"_q).toString()
					<< button.value(u"suggested_username"_q).toString();
			}
			buttons.emplace_back(type, button.value(u"text"_q).toString(),
				HistoryMessageMarkupButton::Visual{
					.iconId = button.value(u"icon_id"_q).toString().toULongLong(),
					.color = HistoryMessageMarkupButton::Color(button.value(u"color"_q).toInt()),
				}, data,
				button.value(u"forward_text"_q).toString(),
				button.value(u"button_id"_q).toString().toLongLong());
			using PeerTypes = decltype(buttons.back().peerTypes);
			buttons.back().peerTypes = PeerTypes::from_raw(
				PeerTypes::Type(button.value(u"peer_types"_q).toInt()));
		}
		result.rows.push_back(std::move(buttons));
	}
	return result;
}

} // namespace

struct LocalQuickReplyView::PreparedPreview {
	LocalQuickReplyMaterialized files;
	QJsonArray descriptors;
	std::map<int, ImageWithLocation> images;
	std::map<int, QByteArray> videos;
	QString error;
};

struct LocalQuickReplyStage::Data {
	QString replyRef;
	QString reference;
	std::vector<LocalQuickReplyMessage> messages;
	std::shared_ptr<const LocalQuickReplyMediaAssets> prepared;
	std::shared_ptr<LocalQuickReplyView::List> list;
};

LocalQuickReplyStage::LocalQuickReplyStage(std::unique_ptr<Data> data)
: _data(std::move(data)) {
}

LocalQuickReplyStage::~LocalQuickReplyStage() = default;

const QString &LocalQuickReplyStage::reference() const {
	return _data->reference;
}

const QString &LocalQuickReplyStage::replyRef() const {
	return _data->replyRef;
}

const std::vector<LocalQuickReplyMessage> &LocalQuickReplyStage::messages() const {
	return _data->messages;
}

auto LocalQuickReplyStage::preparedAssets() const
-> std::shared_ptr<const LocalQuickReplyMediaAssets> {
	return _data->prepared;
}

bool LocalQuickReplyStage::valid() const {
	return _data->list->valid.load();
}

LocalQuickReplyView::List::~List() {
	for (auto &[id, entry] : entries) {
		if (entry.cancelled) entry.cancelled->store(true);
	}
}

LocalQuickReplyView::LocalQuickReplyView(not_null<Session*> owner)
: _owner(owner) {
	owner->localQuickReplies().changes() | rpl::on_next([=] {
		refresh();
	}, _lifetime);
}

LocalQuickReplyView::~LocalQuickReplyView() {
	clear();
}

void LocalQuickReplyView::clear() {
	_cleared = true;
	_lifetime.destroy();
	_pending.clear();
	for (const auto &list : lists()) {
		list->valid.store(false);
		for (auto &[id, entry] : list->entries) {
			if (entry.cancelled) entry.cancelled->store(true);
		}
		list->entries.clear();
	}
	_stageOwners.clear();
	_staged.clear();
	_lists.clear();
}

void LocalQuickReplyView::request(QString replyRef) {
	if (_cleared || _staged.contains(replyRef)) return;
	auto &list = _lists[replyRef];
	if (!list) list = std::make_shared<List>();
	list->errors.clear();
	refresh();
}

auto LocalQuickReplyView::stage(
		QString replyRef,
		QString stageRef,
		std::vector<LocalQuickReplyMessage> messages,
		std::shared_ptr<const LocalQuickReplyMediaAssets> assets)
-> std::shared_ptr<const LocalQuickReplyStage> {
	const auto uuid = QUuid(stageRef);
	if (_cleared || uuid.isNull()
		|| uuid.toString(QUuid::WithoutBraces) != stageRef
		|| stageRef == replyRef || _lists.contains(stageRef)
		|| _staged.contains(stageRef)
		|| _owner->localQuickReplies().find(stageRef)) {
		return nullptr;
	}
	auto ids = base::flat_set<QString>();
	for (const auto &message : messages) {
		if (message.id.isEmpty() || message.replyRef != replyRef
			|| !ids.emplace(message.id).second) return nullptr;
	}
	auto data = std::make_unique<LocalQuickReplyStage::Data>();
	data->replyRef = std::move(replyRef);
	data->reference = stageRef;
	data->messages = std::move(messages);
	data->prepared = std::move(assets);
	data->list = std::make_shared<List>();
	const auto weak = QPointer(this);
	auto result = std::shared_ptr<const LocalQuickReplyStage>(
		new LocalQuickReplyStage(std::move(data)),
		[=](const LocalQuickReplyStage *stage) {
			crl::on_main([=] {
				delete stage;
				if (weak) weak->releaseStage(stageRef);
			});
		});
	_staged.emplace(stageRef, Staged{ result, result->_data->list });
	_stageOwners.emplace(stageRef, result);
	refreshList(stageRef, *result->_data->list, &result->_data->messages);
	return result;
}

auto LocalQuickReplyView::stageLease(not_null<const HistoryItem*> item) const
-> std::shared_ptr<const LocalQuickReplyStage> {
	if (_cleared || !item->isLocalQuickReply()) return nullptr;
	for (const auto &[_, registered] : _staged) {
		const auto stage = registered.stage.lock();
		if (!stage || !stage->valid()) continue;
		for (const auto &[id, entry] : stage->_data->list->entries) {
			if (entry.item.get() == item) return stage;
		}
	}
	return nullptr;
}

void LocalQuickReplyView::clearStage(QString stageRef) {
	_stageOwners.erase(stageRef);
}

void LocalQuickReplyView::releaseStage(const QString &stageRef) {
	for (auto i = _pending.begin(); i != _pending.end();) {
		if (i->first.first == stageRef) i = _pending.erase(i);
		else ++i;
	}
	_staged.erase(stageRef);
	if (!_cleared) _updates.fire_copy(stageRef);
}

auto LocalQuickReplyView::findList(const QString &reference) const
-> std::shared_ptr<List> {
	if (_cleared) return nullptr;
	const auto canonical = _lists.find(reference);
	if (canonical != _lists.end()) return canonical->second;
	const auto i = _staged.find(reference);
	if (i == _staged.end()) return nullptr;
	const auto stage = i->second.stage.lock();
	return stage && stage->valid() ? stage->_data->list : nullptr;
}

auto LocalQuickReplyView::lists() const
-> std::vector<std::shared_ptr<List>> {
	auto result = std::vector<std::shared_ptr<List>>();
	result.reserve(_lists.size() + _staged.size());
	for (const auto &[_, list] : _lists) result.push_back(list);
	for (const auto &[_, registered] : _staged) {
		if (const auto list = registered.list.lock()) result.push_back(list);
	}
	return result;
}

void LocalQuickReplyView::refresh() {
	if (_cleared) return;
	for (auto &[reference, list] : _lists) {
		const auto reply = _owner->localQuickReplies().find(reference);
		refreshList(reference, *list, reply ? &reply->messages : nullptr);
	}
}

void LocalQuickReplyView::refreshList(
		const QString &reference,
		List &list,
		const std::vector<LocalQuickReplyMessage> *messages) {
	auto retained = base::flat_set<QString>();
	list.order.clear();
	if (messages) {
		for (const auto &message : *messages) {
			retained.emplace(message.id);
			list.order.push_back(message.id);
			auto &entry = list.entries[message.id];
			entry.source = message;
			if (!entry.item) {
				const auto history = _owner->history(_owner->session().userPeerId());
				entry.item.reset(history->makeMessage(HistoryItemCommonFields{
					.id = _owner->nextLocalMessageId(),
					.flags = MessageFlag::Outgoing | MessageFlag::Local
						| MessageFlag::HasFromId | MessageFlag::HideDisplayDate,
					.from = _owner->session().userPeerId(),
					.localQuickReply = true,
				}, TextWithEntities(), MTP_messageMediaEmpty()));
			}
			const auto revision = Revision(message);
			if (entry.ready && entry.renderedRevision == revision) continue;
			const auto error = list.errors.find(message.id);
			if (error != list.errors.end() && error->second.revision == revision) continue;
			if (entry.preparingRevision == revision
				&& _pending.contains({ reference, message.id })) continue;
			entry.ready = false;
			entry.preparingRevision = revision;
			list.errors.erase(message.id);
			prepare(reference, message);
		}
	}
	for (auto i = list.entries.begin(); i != list.entries.end();) {
		if (!retained.contains(i->first)) {
			_pending.erase({ reference, i->first });
			if (i->second.cancelled) i->second.cancelled->store(true);
			i = list.entries.erase(i);
		} else ++i;
	}
	for (auto i = list.errors.begin(); i != list.errors.end();) {
		if (!retained.contains(i->first)) i = list.errors.erase(i);
		else ++i;
	}
	updateRelations(list);
	_updates.fire_copy(reference);
}

void LocalQuickReplyView::prepare(QString replyRef, LocalQuickReplyMessage message) {
	const auto list = findList(replyRef);
	if (!list) return;
	auto &entry = list->entries[message.id];
	if (entry.cancelled) entry.cancelled->store(true);
	const auto cancelled = std::make_shared<std::atomic<bool>>(false);
	entry.cancelled = cancelled;
	const auto token = ++_nextToken;
	_pending.insert_or_assign(std::pair(replyRef, message.id), token);
	const auto weak = QPointer(this);
	const auto staged = _staged.find(replyRef);
	const auto stage = staged == _staged.end() ? nullptr : staged->second.stage.lock();
	const auto lease = stage ? stage->_data->prepared : nullptr;
	_owner->localQuickReplies().materializeMessage(message,
		[=](LocalQuickReplyMaterialized files, QString error) {
			if (!weak || weak->_cleared || cancelled->load()) return;
			auto preview = std::make_shared<PreparedPreview>();
			preview->files = std::move(files);
			preview->error = std::move(error);
			preview->descriptors = message.attachments;
			if (lease) {
				for (const auto &path : message.sourcePaths) {
					const auto i = ranges::find(lease->files(), path, &LocalQuickReplyMediaAsset::path);
					if (i == lease->files().end()) continue;
					preview->descriptors.push_back(QJsonObject{
						{ u"filename"_q, i->filename }, { u"mime"_q, i->mime },
						{ u"size"_q, QString::number(i->size) },
					});
				}
			}
			const auto side = PhotoData::SideLimit();
			PreviewQueue().async([=] {
				if (cancelled->load()) return;
				auto images = base::flat_set<int>();
				auto videos = base::flat_set<int>();
				const auto collect = [&](const auto &self, const QJsonObject &media, int depth) -> void {
					if (depth > 16 || !preview->error.isEmpty()) return;
					const auto add = [&](const QString &role, bool image, bool video) {
						if (!media.contains(role)) return;
						const auto index = AssetIndex(media, role);
						if (index < 0 || index >= preview->files.paths.size()) {
							preview->error = u"The media asset reference is invalid."_q;
						} else if (image) images.emplace(index);
						else if (video) videos.emplace(index);
					};
					add(u"asset"_q, media.value(u"kind"_q).toString() == u"photo"_q, false);
					add(u"thumbnail_asset"_q, true, false);
					add(u"good_thumbnail_asset"_q, true, false);
					add(u"video_thumbnail_asset"_q, false, true);
					add(u"video_asset"_q, false, true);
					for (const auto &role : { u"cover_asset_media"_q,
						u"photo_asset_media"_q, u"document_asset_media"_q }) {
						if (media.contains(role)) self(self, media.value(role).toObject(), depth + 1);
					}
					for (const auto &asset : media.value(u"rich_assets"_q).toArray()) {
						self(self, asset.toObject().value(u"media"_q).toObject(), depth + 1);
					}
				};
				collect(collect, message.media, 0);
				auto memory = int64(0);
				for (const auto index : images) {
					if (cancelled->load()) return;
					if (!preview->error.isEmpty()) break;
					auto reader = QImageReader(preview->files.paths[index]);
					reader.setAutoTransform(true);
					const auto size = reader.size();
					const auto scaled = size.width() > side || size.height() > side
						? size.scaled(side, side, Qt::KeepAspectRatio) : size;
					auto input = QFile(preview->files.paths[index]);
					const auto imageBytes = int64(scaled.width()) * scaled.height() * 4;
					if (size.isEmpty() || !input.open(QIODevice::ReadOnly)
						|| memory + imageBytes + input.size() > kPreviewMemoryLimit) {
						preview->error = u"The quick reply preview exceeds the image memory limit."_q;
						break;
					}
					auto bytes = input.read(kPreviewMemoryLimit - memory - imageBytes + 1);
					if (bytes.size() != input.size()
						|| memory + imageBytes + bytes.size() > kPreviewMemoryLimit) {
						preview->error = u"Could not read the quick reply image bytes."_q;
						break;
					}
					if (size.width() > side || size.height() > side) reader.setScaledSize(scaled);
					const auto image = reader.read();
					if (image.isNull()) {
						preview->error = u"Could not read the quick reply image."_q;
						break;
					}
					auto location = Images::FromImageInMemory(image, "JPG", std::move(bytes));
					memory += image.sizeInBytes() + location.bytes.size();
					preview->images.emplace(index, std::move(location));
				}
				for (const auto index : videos) {
					if (cancelled->load()) return;
					if (!preview->error.isEmpty()) break;
					auto input = QFile(preview->files.paths[index]);
					if (!input.open(QIODevice::ReadOnly) || input.size() > kPreviewMemoryLimit - memory) {
						preview->error = u"The quick reply video preview exceeds the memory limit."_q;
						break;
					}
					auto bytes = input.read(kPreviewMemoryLimit - memory + 1);
					memory += bytes.size();
					if (bytes.size() != input.size() || memory > kPreviewMemoryLimit) {
						preview->error = u"Could not read the quick reply video preview."_q;
						break;
					}
					preview->videos.emplace(index, std::move(bytes));
				}
				if (memory > kPreviewMemoryLimit) preview->error = u"The quick reply preview exceeds the memory limit."_q;
				crl::on_main([=] {
					if (weak) weak->ready(replyRef, message, token, preview);
				});
			});
		}, lease);
}

void LocalQuickReplyView::ready(
		QString replyRef,
		LocalQuickReplyMessage message,
		uint64 token,
		std::shared_ptr<PreparedPreview> preview) {
	const auto pending = _pending.find({ replyRef, message.id });
	if (_cleared || pending == _pending.end() || pending->second != token) return;
	_pending.erase(pending);
	const auto list = findList(replyRef);
	if (!list) return;
	const auto found = list->entries.find(message.id);
	if (found == list->entries.end()
		|| Revision(found->second.source) != Revision(message)) return;
	auto &entry = found->second;
	const auto failed = [&](QString error) {
		entry.ready = false;
		list->errors.insert_or_assign(message.id, PreparationError{
			.revision = Revision(message), .text = std::move(error),
		});
		updateRelations(*list);
		_updates.fire_copy(replyRef);
	};
	if (!preview->error.isEmpty()) { failed(preview->error); return; }
	const auto valid = [&](int index) {
		return index >= 0 && index < preview->files.paths.size()
			&& index < preview->descriptors.size();
	};
	auto photos = std::map<int, PhotoData*>();
	auto documents = std::map<int, DocumentData*>();
	auto photoViews = std::vector<std::shared_ptr<PhotoMedia>>();
	auto documentViews = std::vector<std::shared_ptr<DocumentMedia>>();
	auto error = QString();
	const auto photo = [&](const QJsonObject &media) -> PhotoData* {
		const auto index = AssetIndex(media, u"asset"_q);
		const auto image = preview->images.find(index);
		if (!valid(index) || image == preview->images.end()
			|| media.value(u"kind"_q).toString() != u"photo"_q) {
			error = u"The quick reply photo asset is invalid."_q;
			return nullptr;
		}
		if (const auto existing = photos.find(index); existing != photos.end()) return existing->second;
		const auto result = _owner->photo(base::RandomValue<PhotoId>());
		result->setFields(0, !media.value(u"attached_stickers"_q).toArray().isEmpty());
		const auto videoIndex = AssetIndex(media, u"video_asset"_q);
		const auto video = preview->videos.find(videoIndex);
		auto videoLocation = ImageWithLocation();
		if (video != preview->videos.end()) {
			videoLocation = ImageWithLocation{
				.location = ImageLocation(DownloadLocation{ InMemoryLocation{ video->second } },
					image->second.preloaded.width(), image->second.preloaded.height()),
				.bytes = video->second,
				.bytesCount = int(video->second.size()),
			};
		}
		const auto &location = image->second;
		result->updateImages({}, location, location, location, {}, videoLocation,
			crl::time(media.value(u"video_start_ms"_q).toDouble()));
		result->setLocation(Core::FileLocation(preview->files.paths[index]));
		auto view = result->createMediaView();
		view->set(PhotoSize::Large, PhotoSize::Large, location.preloaded, location.bytes);
		if (video != preview->videos.end()) view->setVideo(PhotoSize::Large, video->second);
		photoViews.push_back(std::move(view));
		photos.emplace(index, result);
		return result;
	};
	const auto document = [&](const QJsonObject &media) -> DocumentData* {
		const auto index = AssetIndex(media, u"asset"_q);
		if (!valid(index) || !IsDocument(media.value(u"kind"_q).toString())) {
			error = u"The quick reply document asset is invalid."_q;
			return nullptr;
		}
		if (const auto existing = documents.find(index); existing != documents.end()) return existing->second;
		const auto attachment = preview->descriptors.at(index).toObject();
		const auto thumbnail = preview->images.find(AssetIndex(media, u"thumbnail_asset"_q));
		const auto video = preview->videos.find(AssetIndex(media, u"video_thumbnail_asset"_q));
		auto videoLocation = ImageWithLocation();
		if (video != preview->videos.end()) {
			const auto size = thumbnail != preview->images.end()
				? thumbnail->second.preloaded.size()
				: QSize(media.value(u"width"_q).toInt(), media.value(u"height"_q).toInt());
			videoLocation = ImageWithLocation{
				.location = ImageLocation(DownloadLocation{ InMemoryLocation{ video->second } },
					size.width(), size.height()),
				.bytes = video->second,
				.bytesCount = int(video->second.size()),
			};
		}
		const auto result = _owner->document(base::RandomValue<DocumentId>(), 0, {}, 0,
			Attributes(media, attachment), attachment.value(u"mime"_q).toString(), {},
			thumbnail == preview->images.end() ? ImageWithLocation() : thumbnail->second,
			videoLocation, false, 0, attachment.value(u"size"_q).toString().toLongLong());
		result->setLocation(Core::FileLocation(preview->files.paths[index]));
		auto view = result->createMediaView();
		if (thumbnail != preview->images.end()) view->setThumbnail(thumbnail->second.preloaded);
		const auto good = preview->images.find(AssetIndex(media, u"good_thumbnail_asset"_q));
		if (good != preview->images.end()) view->setGoodThumbnail(good->second.preloaded);
		if (video != preview->videos.end()) view->setVideoThumbnail(video->second);
		documentViews.push_back(std::move(view));
		documents.emplace(index, result);
		return result;
	};
	const auto item = not_null(entry.item.get());
	const auto &media = message.media;
	const auto kind = media.value(u"kind"_q).toString();
	auto content = HistoryMessageContent();
	content.text = message.message;
	content.invertMedia = media.value(u"invert_caption"_q).toBool();
	content.markup = Markup(media.value(u"reply_markup"_q).toObject(), &error);
	if (kind == u"rich_message"_q) {
		auto descriptors = std::map<int, QJsonObject>();
		for (const auto &value : media.value(u"rich_assets"_q).toArray()) {
			const auto asset = value.toObject();
			const auto index = AssetIndex(asset, u"asset"_q);
			const auto descriptor = asset.value(u"media"_q).toObject();
			if (!valid(index) || index != AssetIndex(descriptor, u"asset"_q)
				|| !descriptors.emplace(index, descriptor).second) {
				error = u"The rich media asset descriptor is invalid."_q;
				break;
			}
		}
		if (error.isEmpty()) {
			auto decoded = DecodeLocalQuickReplyRich(media.value(u"rich"_q).toObject(),
				[&](int index) -> PhotoData* {
					const auto i = descriptors.find(index);
					return i == descriptors.end() ? nullptr : photo(i->second);
				}, [&](int index) -> DocumentData* {
					const auto i = descriptors.find(index);
					return i == descriptors.end() ? nullptr : document(i->second);
				});
			if (const auto problem = std::get_if<QString>(&decoded)) error = *problem;
			else content.richPage = std::get<std::shared_ptr<Iv::RichPage>>(std::move(decoded));
		}
	} else if (kind == u"photo"_q) {
		if (const auto data = photo(media)) {
			content.media = std::make_unique<MediaPhoto>(item, data, MediaPhoto::Args{
				.ttlSeconds = media.value(u"ttl_seconds"_q).toInt(),
				.spoiler = media.value(u"spoiler"_q).toBool(),
			});
		}
	} else if (IsDocument(kind)) {
		const auto data = document(media);
		auto cover = static_cast<PhotoData*>(nullptr);
		if (media.contains(u"cover_asset"_q)) {
			const auto descriptor = media.value(u"cover_asset_media"_q).toObject();
			if (AssetIndex(descriptor, u"asset"_q) != AssetIndex(media, u"cover_asset"_q)) {
				error = u"The video cover asset reference is invalid."_q;
			} else cover = photo(descriptor);
		}
		if (data && error.isEmpty()) {
			content.media = std::make_unique<MediaFile>(item, data, MediaFile::Args{
				.ttlSeconds = media.value(u"ttl_seconds"_q).toInt(),
				.videoCover = cover,
				.videoTimestamp = TimeId(media.value(u"video_timestamp"_q).toDouble() / 1000.),
				.spoiler = media.value(u"spoiler"_q).toBool(),
			});
		}
	} else if (kind == u"contact"_q) {
		content.media = std::make_unique<MediaContact>(item,
			UserId(media.value(u"user_id"_q).toString().toULongLong()),
			media.value(u"first_name"_q).toString(), media.value(u"last_name"_q).toString(),
			media.value(u"phone_number"_q).toString(),
			SharedContact::ParseVcard(media.value(u"vcard"_q).toString()));
	} else if (kind == u"geo"_q) {
		content.media = HistoryItem::CreateMedia(item, MTP_messageMediaGeo(GeoPoint(media)));
	} else if (kind == u"live_geo"_q) {
		using Flag = MTPDmessageMediaGeoLive::Flag;
		content.media = HistoryItem::CreateMedia(item, MTP_messageMediaGeoLive(MTP_flags(
			(media.contains(u"heading"_q) ? Flag::f_heading : Flag())
			| (media.contains(u"proximity_notification_radius"_q) ? Flag::f_proximity_notification_radius : Flag())),
			GeoPoint(media), MTP_int(media.value(u"heading"_q).toInt()),
			MTP_int(media.value(u"period"_q).toInt()),
			MTP_int(media.value(u"proximity_notification_radius"_q).toInt())));
	} else if (kind == u"venue"_q) {
		content.media = HistoryItem::CreateMedia(item, MTP_messageMediaVenue(GeoPoint(media),
			MTP_string(media.value(u"title"_q).toString()),
			MTP_string(media.value(u"address"_q).toString()),
			MTP_string(media.value(u"provider"_q).toString()),
			MTP_string(media.value(u"venue_id"_q).toString()),
			MTP_string(media.value(u"venue_type"_q).toString())));
	} else if (kind == u"game"_q || kind == u"invoice"_q) {
		auto image = static_cast<PhotoData*>(nullptr);
		auto animation = static_cast<DocumentData*>(nullptr);
		if (media.contains(u"photo_asset"_q)) {
			const auto descriptor = media.value(u"photo_asset_media"_q).toObject();
			if (AssetIndex(descriptor, u"asset"_q) != AssetIndex(media, u"photo_asset"_q)) {
				error = u"The composite photo asset reference is invalid."_q;
			} else image = photo(descriptor);
		}
		if (media.contains(u"document_asset"_q)) {
			const auto descriptor = media.value(u"document_asset_media"_q).toObject();
			if (AssetIndex(descriptor, u"asset"_q) != AssetIndex(media, u"document_asset"_q)) {
				error = u"The composite document asset reference is invalid."_q;
			} else animation = document(descriptor);
		}
		if (kind == u"game"_q) {
			const auto game = _owner->game(base::RandomValue<GameId>());
			game->shortName = media.value(u"short_name"_q).toString();
			game->title = media.value(u"title"_q).toString();
			game->description = media.value(u"description"_q).toString();
			game->photo = image;
			game->document = animation;
			content.media = std::make_unique<MediaGame>(item, game);
		} else {
			auto invoice = Invoice();
			invoice.amount = media.value(u"total_amount"_q).toString().toULongLong();
			invoice.currency = media.value(u"currency"_q).toString();
			invoice.title = media.value(u"title"_q).toString();
			invoice.description = { media.value(u"description"_q).toString() };
			invoice.photo = image;
			invoice.isTest = media.value(u"test"_q).toBool();
			content.media = std::make_unique<MediaInvoice>(item, invoice);
		}
	} else if (kind == u"text"_q && media.contains(u"webpage"_q)
		&& !media.value(u"no_webpage"_q).toBool()) {
		const auto source = media.value(u"webpage"_q).toObject();
		const auto page = _owner->webpage(base::RandomValue<WebPageId>());
		page->type = WebPageType::Article;
		page->url = source.value(u"url"_q).toString();
		if (page->url.isEmpty()) error = u"The stored webpage URL is missing."_q;
		page->displayUrl = page->url;
		page->title = page->url;
		auto flags = MediaWebPageFlags();
		if (source.value(u"force_large_media"_q).toBool()) flags |= MediaWebPageFlag::ForceLargeMedia;
		if (source.value(u"force_small_media"_q).toBool()) flags |= MediaWebPageFlag::ForceSmallMedia;
		if (source.value(u"manual"_q).toBool()) flags |= MediaWebPageFlag::Manual;
		if (source.value(u"safe"_q).toBool()) flags |= MediaWebPageFlag::Safe;
		content.media = std::make_unique<MediaWebPage>(item, page, flags);
	} else if (kind != u"text"_q) {
		error = u"This message contains media from a newer client; its original data is preserved."_q;
	}
	if (!error.isEmpty()) { failed(std::move(error)); return; }
	entry.item->applyContent(std::move(content));
	entry.documents = std::move(documentViews);
	entry.photos = std::move(photoViews);
	entry.assets = preview->files.assets;
	if (_staged.contains(replyRef) && entry.assets) list->assets.push_back(entry.assets);
	entry.renderedRevision = Revision(message);
	entry.ready = true;
	list->errors.erase(message.id);
	auto limitation = media.value(u"replay_limitation"_q).toString();
	if (media.contains(u"webpage"_q) && !media.value(u"no_webpage"_q).toBool()) {
		limitation = u"The stored link is previewed locally; webpage details require native resolution when sending."_q;
	}
	if (!limitation.isEmpty()) {
		list->errors.emplace(message.id, PreparationError{ Revision(message), limitation });
	}
	updateRelations(*list);
	_updates.fire_copy(replyRef);
}

void LocalQuickReplyView::updateRelations(List &list) {
	for (auto &[id, entry] : list.entries) {
		auto group = uint64(0);
		if (entry.ready && !entry.source.groupRef.isEmpty()) {
			auto &value = list.groups[entry.source.groupRef];
			if (!value) value = base::RandomValue<uint64>() | uint64(1);
			group = value;
		}
		auto reply = FullReplyTo();
		const auto target = list.entries.find(entry.source.replyToRef);
		if (entry.ready && target != list.entries.end()
			&& target->second.ready && target->first != id) {
			reply.messageId = target->second.item->fullId();
		}
		entry.item->updateLocalQuickReplyRelations(group, std::move(reply));
	}
}

FullMsgId LocalQuickReplyView::resolve(
		const QString &replyRef,
		const QString &messageRef) const {
	const auto list = findList(replyRef);
	if (!list) return {};
	const auto entry = list->entries.find(messageRef);
	return entry == list->entries.end() || !entry->second.ready
		? FullMsgId() : entry->second.item->fullId();
}

MessagesSlice LocalQuickReplyView::slice(const QString &replyRef) const {
	auto result = MessagesSlice();
	result.skippedBefore = result.skippedAfter = 0;
	const auto list = findList(replyRef);
	if (!list) return result;
	for (const auto &id : list->order) {
		const auto item = list->entries.find(id);
		if (item != list->entries.end() && item->second.ready) {
			result.ids.push_back(item->second.item->fullId());
		}
	}
	result.fullCount = int(result.ids.size());
	return result;
}

std::optional<LocalQuickReplyMessage> LocalQuickReplyView::message(
		not_null<const HistoryItem*> item) const {
	if (_cleared || !item->isLocalQuickReply()) return std::nullopt;
	for (const auto &list : lists()) {
		for (const auto &[id, entry] : list->entries) {
			if (entry.item.get() == item) return entry.source;
		}
	}
	return std::nullopt;
}

bool LocalQuickReplyView::less(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) const {
	if (_cleared) return a->id < b->id;
	for (const auto &list : lists()) {
		auto first = -1;
		auto second = -1;
		for (auto i = 0; i != int(list->order.size()); ++i) {
			const auto entry = list->entries.find(list->order[i]);
			if (entry == list->entries.end()) continue;
			if (entry->second.item.get() == a) first = i;
			if (entry->second.item.get() == b) second = i;
		}
		if (first >= 0 && second >= 0) return first < second;
	}
	return a->id < b->id;
}

rpl::producer<> LocalQuickReplyView::updates(const QString &replyRef) const {
	return _updates.events() | rpl::filter([=](const QString &reference) {
		return reference == replyRef;
	}) | rpl::to_empty;
}

QString LocalQuickReplyView::error(const QString &replyRef) const {
	const auto list = findList(replyRef);
	if (!list) return {};
	auto errors = QStringList();
	for (const auto &id : list->order) {
		const auto error = list->errors.find(id);
		if (error != list->errors.end()) {
			errors.push_back(id + u": "_q + error->second.text);
		}
	}
	return errors.join(QChar(10));
}

} // namespace Data
