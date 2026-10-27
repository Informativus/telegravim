/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/local_quick_replies.h"
#include "history/history_item.h"

#include <QtGui/QImage>

#include <atomic>
#include <map>

namespace Data {

class DocumentMedia;
class PhotoMedia;
class Session;
struct MessagesSlice;

/*
A stage is an immutable insertion snapshot. Hold its lease while a draft or an
outgoing operation uses native items or their files. clearStage releases only
the view's draft reference; final release destroys native items on the main
thread. Session clear invalidates every stage and destroys native items before
the owner dies; source records and file leases may safely outlive that point.
*/
class LocalQuickReplyStage final {
public:
	~LocalQuickReplyStage();
	[[nodiscard]] const QString &reference() const;
	[[nodiscard]] const QString &replyRef() const;
	[[nodiscard]] const std::vector<LocalQuickReplyMessage> &messages() const;
	[[nodiscard]] auto preparedAssets() const
		-> std::shared_ptr<const LocalQuickReplyMediaAssets>;
	[[nodiscard]] bool valid() const;

private:
	friend class LocalQuickReplyView;
	struct Data;
	explicit LocalQuickReplyStage(std::unique_ptr<Data> data);
	const std::unique_ptr<Data> _data;

};

class LocalQuickReplyView final : public QObject {
public:
	explicit LocalQuickReplyView(not_null<Session*> owner);
	~LocalQuickReplyView();

	void clear();
	void request(QString replyRef);
	[[nodiscard]] std::shared_ptr<const LocalQuickReplyStage> stage(
		QString replyRef,
		QString stageRef,
		std::vector<LocalQuickReplyMessage> messages,
		std::shared_ptr<const LocalQuickReplyMediaAssets> assets = nullptr);
	[[nodiscard]] std::shared_ptr<const LocalQuickReplyStage> stageLease(
		not_null<const HistoryItem*> item) const;
	void clearStage(QString stageRef);
	[[nodiscard]] FullMsgId resolve(const QString &replyRef,
		const QString &messageRef) const;
	[[nodiscard]] MessagesSlice slice(const QString &replyRef) const;
	[[nodiscard]] std::optional<LocalQuickReplyMessage> message(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool less(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) const;
	[[nodiscard]] rpl::producer<> updates(const QString &replyRef) const;
	[[nodiscard]] QString error(const QString &replyRef) const;

private:
	friend class LocalQuickReplyStage;
	using OwnedItem = std::unique_ptr<HistoryItem, HistoryItem::Destroyer>;
	struct PreparedPreview;
	struct Entry {
		LocalQuickReplyMessage source;
		std::shared_ptr<const LocalQuickReplyAssetLease> assets;
		OwnedItem item;
		std::vector<std::shared_ptr<DocumentMedia>> documents;
		std::vector<std::shared_ptr<PhotoMedia>> photos;
		QByteArray renderedRevision;
		QByteArray preparingRevision;
		std::shared_ptr<std::atomic<bool>> cancelled;
		bool ready = false;
	};
	struct PreparationError {
		QByteArray revision;
		QString text;
	};
	struct List {
		~List();
		std::atomic<bool> valid = true;
		std::vector<std::shared_ptr<const LocalQuickReplyAssetLease>> assets;
		std::map<QString, Entry> entries;
		std::vector<QString> order;
		std::map<QString, PreparationError> errors;
		std::map<QString, uint64> groups;
	};
	struct Staged {
		std::weak_ptr<const LocalQuickReplyStage> stage;
		std::weak_ptr<List> list;
	};
	[[nodiscard]] std::shared_ptr<List> findList(const QString &reference) const;
	[[nodiscard]] std::vector<std::shared_ptr<List>> lists() const;
	void refresh();
	void refreshList(const QString &reference, List &list,
		const std::vector<LocalQuickReplyMessage> *messages);
	void releaseStage(const QString &stageRef);
	void prepare(QString replyRef, LocalQuickReplyMessage message);
	void ready(QString replyRef, LocalQuickReplyMessage message,
		uint64 token, std::shared_ptr<PreparedPreview> preview);
	void updateRelations(List &list);

	const not_null<Session*> _owner;
	std::map<QString, std::shared_ptr<List>> _lists;
	std::map<QString, Staged> _staged;
	std::map<QString, std::shared_ptr<const LocalQuickReplyStage>> _stageOwners;
	std::map<std::pair<QString, QString>, uint64> _pending;
	uint64 _nextToken = 0;
	bool _cleared = false;
	rpl::event_stream<QString> _updates;
	rpl::lifetime _lifetime;

};

} // namespace Data
