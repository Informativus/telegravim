/*
This file is part of Telegram Desktop, the official desktop application
for the Telegram messaging service.
*/
#pragma once

#include "data/data_types.h"
#include "rpl/producer.h"

#include <QtCore/QString>
#include <optional>
#include <vector>

class QByteArray;
class QString;
class QStringView;

namespace Main {
class Session;
} // namespace Main

namespace E2E {

struct ReceivedMessage {
	QString text;
	std::vector<QString> replies;
};

class Chat final {
public:
	explicit Chat(not_null<Main::Session*> session);
	~Chat();

	Chat(const Chat &) = delete;
	Chat &operator=(const Chat &) = delete;

	[[nodiscard]] QByteArray publicKey() const;
	[[nodiscard]] QString publicKeyBase64() const;
	[[nodiscard]] QString fingerprint() const;
	[[nodiscard]] QString peerPublicKeyBase64(PeerId peerId) const;
	[[nodiscard]] QString peerFingerprint(PeerId peerId) const;
	[[nodiscard]] QString keyExchangeMessage() const;
	[[nodiscard]] QString stateMessage(PeerId peerId, bool enabled);
	[[nodiscard]] QString controlNotice(
		PeerId peerId,
		QStringView control,
		bool outgoing,
		bool live = false);

	[[nodiscard]] bool hasPeerKey(PeerId peerId) const;
	[[nodiscard]] bool enabled(PeerId peerId) const;
	[[nodiscard]] bool ready(PeerId peerId) const;
	[[nodiscard]] bool isEncrypted(PeerId peerId, MsgId messageId) const;
	[[nodiscard]] rpl::producer<PeerId> changes() const;
	[[nodiscard]] bool setPeerKey(PeerId peerId, const QByteArray &publicKey);
	[[nodiscard]] bool replacePeerKey(
		PeerId peerId,
		const QByteArray &publicKey);
	[[nodiscard]] bool setEnabled(PeerId peerId, bool enabled);
	[[nodiscard]] bool disable(PeerId peerId);

	[[nodiscard]] std::optional<QString> encrypt(
		PeerId peerId,
		QStringView plaintext);
	[[nodiscard]] std::optional<ReceivedMessage> receive(
		PeerId peerId,
		MsgId messageId,
		bool outgoing,
		bool live,
		QStringView envelope);

	[[nodiscard]] static bool IsEnvelope(QStringView text);
	[[nodiscard]] static bool IsKeyExchange(QStringView text);
	[[nodiscard]] static bool IsControlMessage(QStringView text);
	[[nodiscard]] static int PlaintextLimit(int envelopeLimit);

private:
	struct PeerState;
	struct Identity;

	[[nodiscard]] Identity *identity();
	[[nodiscard]] PeerState *peer(PeerId peerId);
	[[nodiscard]] const PeerState *peer(PeerId peerId) const;
	[[nodiscard]] bool save();
	[[nodiscard]] std::optional<QString> encryptFrame(
		PeerId peerId,
		QStringView kind,
		QStringView plaintext);

	class Private;
	const std::unique_ptr<Private> _private;

};

} // namespace E2E
