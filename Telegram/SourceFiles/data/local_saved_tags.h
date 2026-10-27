/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"
#include "data/data_message_reaction_id.h"

#include <rpl/event_stream.h>

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QStringList>

#include <deque>
#include <map>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Storage::LocalData {
class Store;
struct Result;
} // namespace Storage::LocalData

namespace Data {

struct LocalSavedTag {
	QString id;
	QString title;
	QString emoji;
	ReactionId reaction;
	int position = 0;
	int count = 0;
	friend bool operator==(const LocalSavedTag &, const LocalSavedTag &) = default;
};

struct LocalTaggedMessage {
	FullMsgId id;
	QString text;
	QStringList tags;
	std::optional<PeerId> savedPeer;
	friend bool operator==(
		const LocalTaggedMessage &,
		const LocalTaggedMessage &) = default;
};

struct LocalSavedTagsFilter {
	QStringList tags;
	QString text;
	friend bool operator==(
		const LocalSavedTagsFilter &,
		const LocalSavedTagsFilter &) = default;
};

class LocalSavedTags final : public QObject {
public:
	LocalSavedTags(QObject *parent, not_null<Main::Session*> session);
	~LocalSavedTags();

	void reload();
	[[nodiscard]] bool busy() const;
	[[nodiscard]] bool loaded() const;
	[[nodiscard]] const QString &error() const;
	[[nodiscard]] const std::vector<LocalSavedTag> &tags() const;
	[[nodiscard]] QString tagId(const ReactionId &reaction) const;
	[[nodiscard]] const LocalSavedTagsFilter &filter() const;
	[[nodiscard]] std::vector<ReactionId> filterReactions() const;
	void rememberFilter(std::vector<ReactionId> tags, QString text);
	[[nodiscard]] std::shared_ptr<const std::vector<LocalTaggedMessage>>
		messagesSnapshot() const;
	[[nodiscard]] uint64 revision() const;
	[[nodiscard]] uint64 viewRevision() const;
	[[nodiscard]] const std::map<FullMsgId, std::vector<MessageReaction>> &
		nativeReactions() const;
	[[nodiscard]] std::optional<PeerId> savedPeer(FullMsgId id) const;
	[[nodiscard]] const std::vector<MessageReaction> &reactions(
		FullMsgId id) const;
	[[nodiscard]] std::vector<ReactionId> chosenReactions(FullMsgId id) const;
	[[nodiscard]] std::vector<LocalTaggedMessage> messages(
		const QStringList &tags) const;
	[[nodiscard]] int assignedCount(
		const QString &tag,
		const std::vector<FullMsgId> &messages) const;
	[[nodiscard]] rpl::producer<> changes() const;
	void toggleReaction(
		FullMsgId message,
		ReactionId reaction,
		Fn<void(bool)> done = nullptr);
	void setReaction(
		std::vector<FullMsgId> messages,
		ReactionId reaction,
		bool assigned,
		Fn<void(bool)> done = nullptr);
	void renameReaction(
		ReactionId reaction,
		QString title,
		Fn<void(bool)> done = nullptr);
	void removeReaction(ReactionId reaction, Fn<void(bool)> done = nullptr);
	void observeMessage(not_null<HistoryItem*> item);

	void saveTag(
		QString id,
		QString title,
		QString emoji,
		Fn<void(bool)> done);
	void removeTag(QString id, Fn<void(bool)> done);
	void moveTag(QString id, int delta, Fn<void(bool)> done);
	void setAssigned(
		QString tag,
		std::vector<FullMsgId> messages,
		bool assigned,
		Fn<void(bool)> done);
	void saveFilter(LocalSavedTagsFilter filter, Fn<void(bool)> done);
	void updateTexts(
		std::vector<LocalTaggedMessage> messages,
		Fn<void(bool)> done);

private:
	struct Mutation {
		Fn<Storage::LocalData::Result(QJsonObject &)> change;
		Fn<void(bool)> done;
		Fn<void()> optimistic;
	};

	void mutate(
		Fn<Storage::LocalData::Result(QJsonObject &)> change,
		Fn<void(bool)> done,
		Fn<void()> optimistic = nullptr);
	void runNextMutation();
	void flushFilter();
	void flushObservedMessages();
	void rebuildVisible();
	void applyReaction(
		const std::vector<FullMsgId> &messages,
		const ReactionId &reaction,
		bool assigned);
	void apply(
		std::vector<LocalSavedTag> tags,
		std::vector<LocalTaggedMessage> messages,
		LocalSavedTagsFilter filter);

	const not_null<Main::Session*> _session;
	std::shared_ptr<Storage::LocalData::Store> _store;
	std::vector<LocalSavedTag> _tags;
	std::shared_ptr<const std::vector<LocalTaggedMessage>> _messages;
	std::vector<LocalSavedTag> _committedTags;
	std::map<ReactionId, QString> _tagIds;
	std::map<FullMsgId, std::vector<MessageReaction>> _reactions;
	std::map<FullMsgId, PeerId> _savedPeers;
	std::deque<Mutation> _mutations;
	LocalSavedTagsFilter _filter;
	std::optional<LocalSavedTagsFilter> _pendingFilter;
	std::vector<ReactionId> _pendingFilterReactions;
	std::map<FullMsgId, LocalTaggedMessage> _observedMessages;
	bool _observationScheduled = false;
	QString _error;
	bool _busy = false;
	bool _loaded = false;
	bool _reloadPending = false;
	uint64 _revision = 0;
	uint64 _viewRevision = 0;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

} // namespace Data
