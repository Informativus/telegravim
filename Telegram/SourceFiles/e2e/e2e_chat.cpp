/*
This file is part of Telegram Desktop, the official desktop application
for the Telegram messaging service.
*/
#include "e2e/e2e_chat.h"

#include "main/main_session.h"
#include "storage/storage_account.h"
#include "rpl/event_stream.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QStringConverter>

#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>

namespace E2E {
namespace {

constexpr auto kIdentityPref = "televim.e2e.identity.v1";
constexpr auto kPeersPref = "televim.e2e.peers.v1";
constexpr char kIdentityVersion[] = "TVI1";
const auto kEnvelopePrefix = u"TVE2."_q;
const auto kKeyExchangePrefix = u"TVE2.KEY."_q;
constexpr auto kInfo = "telegravim-e2e-v2";
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;
constexpr auto kPrivateKeySize = 32;
constexpr auto kPublicKeySize = 65;

using EcKey = std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)>;
using BigNum = std::unique_ptr<BIGNUM, decltype(&BN_free)>;

[[nodiscard]] EcKey NewKey() {
	auto key = EcKey(
		EC_KEY_new_by_curve_name(NID_X9_62_prime256v1),
		&EC_KEY_free);
	return key && EC_KEY_generate_key(key.get()) == 1
		? std::move(key)
		: EcKey(nullptr, &EC_KEY_free);
}

[[nodiscard]] QByteArray PrivateBytes(const EC_KEY *key) {
	const auto privatePart = EC_KEY_get0_private_key(key);
	if (!privatePart) {
		return {};
	}
	auto result = QByteArray(kPrivateKeySize, Qt::Uninitialized);
	return BN_bn2binpad(
		privatePart,
		reinterpret_cast<unsigned char *>(result.data()),
		result.size()) == result.size()
		? result
		: QByteArray();
}

[[nodiscard]] QByteArray PublicBytes(const EC_KEY *key) {
	const auto group = EC_KEY_get0_group(key);
	const auto point = EC_KEY_get0_public_key(key);
	if (!group || !point) {
		return {};
	}
	auto result = QByteArray(kPublicKeySize, Qt::Uninitialized);
	const auto written = EC_POINT_point2oct(
		group,
		point,
		POINT_CONVERSION_UNCOMPRESSED,
		reinterpret_cast<unsigned char *>(result.data()),
		result.size(),
		nullptr);
	return written == result.size() ? result : QByteArray();
}

[[nodiscard]] EcKey LoadPrivate(const QByteArray &bytes) {
	if (bytes.size() != kPrivateKeySize) {
		return EcKey(nullptr, &EC_KEY_free);
	}
	auto key = EcKey(
		EC_KEY_new_by_curve_name(NID_X9_62_prime256v1),
		&EC_KEY_free);
	if (!key) {
		return key;
	}
	auto privatePart = BigNum(
		BN_bin2bn(
			reinterpret_cast<const unsigned char *>(bytes.constData()),
			bytes.size(),
			nullptr),
		&BN_free);
	if (!privatePart || EC_KEY_set_private_key(key.get(), privatePart.get()) != 1) {
		return EcKey(nullptr, &EC_KEY_free);
	}
	auto point = EC_POINT_new(EC_KEY_get0_group(key.get()));
	if (!point || EC_POINT_mul(
			EC_KEY_get0_group(key.get()),
			point,
			privatePart.get(),
			nullptr,
			nullptr,
			nullptr) != 1
		|| EC_KEY_set_public_key(key.get(), point) != 1) {
		if (point) {
			EC_POINT_free(point);
		}
		return EcKey(nullptr, &EC_KEY_free);
	}
	EC_POINT_free(point);
	return EC_KEY_check_key(key.get()) == 1
		? std::move(key)
		: EcKey(nullptr, &EC_KEY_free);
}

[[nodiscard]] QByteArray Sha256(QByteArrayView data) {
	auto result = QByteArray(SHA256_DIGEST_LENGTH, Qt::Uninitialized);
	SHA256(
		reinterpret_cast<const unsigned char *>(data.data()),
		data.size(),
		reinterpret_cast<unsigned char *>(result.data()));
	return result;
}

[[nodiscard]] std::optional<QByteArray> SharedSecret(
		const EC_KEY *local,
		QByteArrayView peerBytes) {
	if (peerBytes.size() != kPublicKeySize) {
		return std::nullopt;
	}
	const auto group = EC_KEY_get0_group(local);
	auto point = std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)>(
		EC_POINT_new(group),
		&EC_POINT_free);
	if (!point || EC_POINT_oct2point(
			group,
			point.get(),
			reinterpret_cast<const unsigned char *>(peerBytes.data()),
			peerBytes.size(),
			nullptr) != 1
		|| EC_POINT_is_on_curve(group, point.get(), nullptr) != 1
		|| EC_POINT_is_at_infinity(group, point.get()) == 1) {
		return std::nullopt;
	}
	auto result = QByteArray(32, Qt::Uninitialized);
	if (ECDH_compute_key(
			reinterpret_cast<unsigned char *>(result.data()),
			result.size(),
			point.get(),
			local,
			nullptr) != result.size()) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] QByteArray Hkdf(
		QByteArrayView secret,
		QByteArrayView info) {
	auto result = QByteArray(32, Qt::Uninitialized);
	auto *context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
	if (!context) {
		return {};
	}
	auto length = size_t(result.size());
	const auto salt = std::array<unsigned char, 32>();
	const auto ok = EVP_PKEY_derive_init(context) == 1
		&& EVP_PKEY_CTX_set_hkdf_md(context, EVP_sha256()) == 1
		&& EVP_PKEY_CTX_set1_hkdf_salt(context, salt.data(), salt.size()) == 1
		&& EVP_PKEY_CTX_set1_hkdf_key(
			context,
			reinterpret_cast<const unsigned char *>(secret.data()),
			secret.size()) == 1
		&& EVP_PKEY_CTX_add1_hkdf_info(
			context,
			reinterpret_cast<const unsigned char *>(info.data()),
			info.size()) == 1
		&& EVP_PKEY_derive(
			context,
			reinterpret_cast<unsigned char *>(result.data()),
			&length) == 1
		&& length == size_t(result.size());
	EVP_PKEY_CTX_free(context);
	if (!ok) {
		OPENSSL_cleanse(result.data(), result.size());
		return {};
	}
	return result;
}

[[nodiscard]] QByteArray DeriveKey(
		const EC_KEY *localKey,
		QByteArrayView localPublic,
		QByteArrayView peerPublic,
		quint64 localId,
		quint64 peerId,
		bool outgoing) {
	const auto shared = SharedSecret(localKey, peerPublic);
	if (!shared) {
		return {};
	}
	const auto localBytes = localPublic.toByteArray();
	const auto peerBytes = peerPublic.toByteArray();
	auto info = QByteArray(kInfo);
	info += '\0';
	info += outgoing ? localBytes : peerBytes;
	info += outgoing ? peerBytes : localBytes;
	info += QByteArray::number(outgoing ? localId : peerId);
	info += ':';
	info += QByteArray::number(outgoing ? peerId : localId);
	return Hkdf(*shared, info);
}

[[nodiscard]] QByteArray AesGcmEncrypt(
		QByteArrayView key,
		QByteArrayView nonce,
		QByteArrayView plaintext,
		QByteArrayView aad) {
	if (key.size() != 32 || nonce.size() != kNonceSize) {
		return {};
	}
	auto *context = EVP_CIPHER_CTX_new();
	if (!context) {
		return {};
	}
	auto result = QByteArray(plaintext.size() + kTagSize, Qt::Uninitialized);
	auto written = 0;
	auto total = 0;
	auto ok = EVP_EncryptInit_ex(
		context,
		EVP_aes_256_gcm(),
		nullptr,
		nullptr,
		nullptr) == 1
		&& EVP_EncryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char *>(key.data()),
			reinterpret_cast<const unsigned char *>(nonce.data())) == 1;
	if (ok && !aad.isEmpty()) {
		ok = EVP_EncryptUpdate(
			context,
			nullptr,
			&written,
			reinterpret_cast<const unsigned char *>(aad.data()),
			aad.size()) == 1;
	}
	ok = ok && EVP_EncryptUpdate(
		context,
		reinterpret_cast<unsigned char *>(result.data()),
		&written,
		reinterpret_cast<const unsigned char *>(plaintext.data()),
		plaintext.size()) == 1;
	total = written;
	ok = ok && EVP_EncryptFinal_ex(
		context,
		reinterpret_cast<unsigned char *>(result.data() + total),
		&written) == 1;
	total += written;
	ok = ok && EVP_CIPHER_CTX_ctrl(
		context,
		EVP_CTRL_GCM_GET_TAG,
		kTagSize,
		result.data() + total) == 1;
	EVP_CIPHER_CTX_free(context);
	if (!ok) {
		OPENSSL_cleanse(result.data(), result.size());
		return {};
	}
	result.resize(total + kTagSize);
	return result;
}

[[nodiscard]] std::optional<QByteArray> AesGcmDecrypt(
		QByteArrayView key,
		QByteArrayView nonce,
		QByteArrayView ciphertext,
		QByteArrayView aad) {
	if (key.size() != 32 || nonce.size() != kNonceSize
		|| ciphertext.size() < kTagSize) {
		return std::nullopt;
	}
	auto *context = EVP_CIPHER_CTX_new();
	if (!context) {
		return std::nullopt;
	}
	const auto body = ciphertext.first(ciphertext.size() - kTagSize);
	const auto tag = ciphertext.last(kTagSize);
	auto result = QByteArray(body.size(), Qt::Uninitialized);
	auto written = 0;
	auto total = 0;
	auto ok = EVP_DecryptInit_ex(
		context,
		EVP_aes_256_gcm(),
		nullptr,
		nullptr,
		nullptr) == 1
		&& EVP_DecryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const unsigned char *>(key.data()),
			reinterpret_cast<const unsigned char *>(nonce.data())) == 1;
	if (ok && !aad.isEmpty()) {
		ok = EVP_DecryptUpdate(
			context,
			nullptr,
			&written,
			reinterpret_cast<const unsigned char *>(aad.data()),
			aad.size()) == 1;
	}
	ok = ok && EVP_DecryptUpdate(
		context,
		reinterpret_cast<unsigned char *>(result.data()),
		&written,
		reinterpret_cast<const unsigned char *>(body.data()),
		body.size()) == 1;
	total = written;
	ok = ok && EVP_CIPHER_CTX_ctrl(
		context,
		EVP_CTRL_GCM_SET_TAG,
		kTagSize,
		const_cast<char *>(tag.data())) == 1;
	ok = ok && EVP_DecryptFinal_ex(
		context,
		reinterpret_cast<unsigned char *>(result.data() + total),
		&written) == 1;
	total += written;
	EVP_CIPHER_CTX_free(context);
	if (!ok) {
		OPENSSL_cleanse(result.data(), result.size());
		return std::nullopt;
	}
	result.resize(total);
	return result;
}

[[nodiscard]] QString FingerprintFor(QByteArrayView publicKey) {
	return QString::fromLatin1(Sha256(publicKey).toHex()).toUpper();
}

[[nodiscard]] QByteArray EncodeState(const QJsonObject &state) {
	return QJsonDocument(state).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QJsonObject DecodeState(const QByteArray &serialized) {
	const auto document = QJsonDocument::fromJson(serialized);
	return document.isObject() ? document.object() : QJsonObject();
}

[[nodiscard]] std::optional<quint64> Number(
		QStringView text,
		bool allowZero = false) {
	auto ok = false;
	const auto result = text.toULongLong(&ok);
	if (!ok || (!allowZero && !result)
		|| result > quint64(std::numeric_limits<qint64>::max())
		|| QString::number(result) != text) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::optional<QByteArray> Base64(QStringView text) {
	const auto result = QByteArray::fromBase64(
		text.toLatin1(),
		QByteArray::AbortOnBase64DecodingErrors);
	if (QString::fromLatin1(result.toBase64()) != text) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] bool ValidPublicKey(QByteArrayView bytes) {
	if (bytes.size() != kPublicKeySize || bytes.front() != '\x04') {
		return false;
	}
	const auto group = std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)>(
		EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1),
		&EC_GROUP_free);
	if (!group) {
		return false;
	}
	const auto point = std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)>(
		EC_POINT_new(group.get()),
		&EC_POINT_free);
	return point
		&& EC_POINT_oct2point(
			group.get(),
			point.get(),
			reinterpret_cast<const unsigned char *>(bytes.data()),
			bytes.size(),
			nullptr) == 1
		&& EC_POINT_is_on_curve(group.get(), point.get(), nullptr) == 1
		&& EC_POINT_is_at_infinity(group.get(), point.get()) == 0;
}

[[nodiscard]] QString NewChallenge() {
	auto bytes = QByteArray(16, Qt::Uninitialized);
	if (RAND_bytes(
			reinterpret_cast<unsigned char *>(bytes.data()),
			bytes.size()) != 1) {
		return {};
	}
	return QString::fromLatin1(bytes.toBase64());
}

[[nodiscard]] bool ValidChallenge(QStringView text) {
	const auto bytes = Base64(text);
	return bytes && bytes->size() == 16;
}

} // namespace

struct Chat::Identity {
	QByteArray privateKey;
	QByteArray publicKey;
};

struct Chat::PeerState {
	struct Receipt {
		MsgId messageId;
		QByteArray digest;
	};
	struct Key {
		QByteArray publicKey;
		quint64 sent = 0;
		quint64 control = 0;
		std::map<quint64, Receipt> incoming;
		std::map<quint64, Receipt> outgoing;
	};
	struct Mode {
		bool requested = false;
		bool ready = false;
		bool keyChanged = false;
		QString challenge;
		QString lastPeerChallenge;

		friend bool operator==(const Mode &, const Mode &) = default;
	};
	QString active;
	Mode mode;
	std::map<QString, Key> keys;
};

class Chat::Private final {
public:
	explicit Private(not_null<Main::Session*> session)
	: session(session) {
		if (!load()) {
			failed = true;
			identity.reset();
			peers.clear();
		}
	}

	not_null<Main::Session*> session;
	std::optional<Identity> identity;
	std::map<PeerId, PeerState> peers;
	rpl::event_stream<PeerId> changes;
	bool failed = false;

	bool loadReceipts(
			const QJsonValue &value,
			std::map<quint64, PeerState::Receipt> &receipts,
			bool outgoing) {
		if (!value.isObject()) {
			return false;
		}
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			const auto sequence = Number(i.key());
			const auto receipt = i.value().toObject();
			const auto id = Number(receipt.value(u"id"_q).toString(), outgoing);
			const auto digest = Base64(receipt.value(u"digest"_q).toString());
			if (!sequence || !id || !digest || digest->size() != 32) {
				return false;
			}
			receipts.emplace(*sequence, PeerState::Receipt{
				MsgId(*id),
				*digest,
			});
		}
		return true;
	}

	QJsonObject receiptsJson(
			const std::map<quint64, PeerState::Receipt> &receipts) const {
		auto result = QJsonObject();
		for (const auto &[sequence, receipt] : receipts) {
			result.insert(QString::number(sequence), QJsonObject{
				{ u"id"_q, QString::number(receipt.messageId.bare) },
				{ u"digest"_q, QString::fromLatin1(receipt.digest.toBase64()) },
			});
		}
		return result;
	}

	bool load() {
		// A snapshot keeps identity, peer-key epochs and receipts together.
		// Only accounts without a snapshot may migrate the old preferences.
		// Selecting an old peer key must retain its counters and receipt ledger;
		// validating both directions prevents a restart from making a replay fresh.
		// The caller discards every loaded record if any part is malformed.
		const auto stored = session->local().readE2eState();
		if (!stored) {
			return false;
		}
		const auto hasSnapshot = !stored->isEmpty();
		const auto snapshot = hasSnapshot ? DecodeState(*stored) : QJsonObject();
		const auto version = hasSnapshot
			? snapshot.value(u"version"_q).toInt()
			: 1;
		auto serialized = session->local().readPref<QByteArray>(
			kIdentityPref,
			QByteArray());
		const auto legacyPeers = session->local().readPref<QByteArray>(
			kPeersPref,
			QByteArray());
		if (!hasSnapshot && !legacyPeers.isEmpty()
			&& !QJsonDocument::fromJson(legacyPeers).isObject()) {
			return false;
		}
		auto state = DecodeState(legacyPeers);
		if (hasSnapshot) {
			const auto decoded = Base64(snapshot.value(u"identity"_q).toString());
			if ((version != 1 && version != 2)
				|| !decoded || decoded->isEmpty()
				|| !snapshot.value(u"peers"_q).isObject()
				|| (version == 2 && snapshot.value(u"account"_q).toString()
					!= QString::number(session->userId().bare))) {
				return false;
			}
			serialized = *decoded;
			state = snapshot.value(u"peers"_q).toObject();
		}
		if (!hasSnapshot && serialized.isEmpty() && state.isEmpty()) {
			return true;
		}
		constexpr auto kPrefixSize = sizeof(kIdentityVersion) - 1;
		if (!serialized.startsWith(kIdentityVersion)
			|| serialized.size() != kPrefixSize + kPrivateKeySize + kPublicKeySize) {
			return false;
		}
		const auto privateKey = serialized.mid(kPrefixSize, kPrivateKeySize);
		const auto local = LoadPrivate(privateKey);
		const auto publicKey = serialized.mid(kPrefixSize + kPrivateKeySize);
		if (!local || PublicBytes(local.get()) != publicKey) {
			return false;
		}
		identity = Identity{ privateKey, publicKey };
		for (auto i = state.begin(); i != state.end(); ++i) {
			const auto id = Number(i.key());
			if (!id || !peerIsUser(PeerId(*id)) || !i.value().isObject()) {
				return false;
			}
			const auto object = i.value().toObject();
			auto peer = PeerState();
			if (version == 1) {
				const auto key = Base64(object.value(u"publicKey"_q).toString());
				if (!key || !ValidPublicKey(*key)
					|| !object.value(u"enabled"_q).isBool()) {
					return false;
				}
				peer.active = FingerprintFor(*key);
				peer.keys[peer.active].publicKey = *key;
				peer.mode.requested = object.value(u"enabled"_q).toBool();
				peer.mode.challenge = peer.mode.requested ? NewChallenge() : QString();
				if (peer.mode.requested && peer.mode.challenge.isEmpty()) {
					return false;
				}
			} else {
				peer.active = object.value(u"active"_q).toString();
				const auto mode = object.value(u"mode"_q).toObject();
				if (!mode.value(u"requested"_q).isBool()
					|| !mode.value(u"ready"_q).isBool()
					|| !mode.value(u"keyChanged"_q).isBool()
					|| !mode.value(u"challenge"_q).isString()
					|| !mode.value(u"lastPeerChallenge"_q).isString()
					|| !object.value(u"keys"_q).isObject()) {
					return false;
				}
				peer.mode = {
					mode.value(u"requested"_q).toBool(),
					mode.value(u"ready"_q).toBool(),
					mode.value(u"keyChanged"_q).toBool(),
					mode.value(u"challenge"_q).toString(),
					mode.value(u"lastPeerChallenge"_q).toString(),
				};
				if ((peer.mode.requested && !ValidChallenge(peer.mode.challenge))
					|| (!peer.mode.challenge.isEmpty() && !ValidChallenge(peer.mode.challenge))
					|| (!peer.mode.lastPeerChallenge.isEmpty()
						&& !ValidChallenge(peer.mode.lastPeerChallenge))
					|| (peer.mode.ready && (!peer.mode.requested || peer.mode.keyChanged))) {
					return false;
				}
				const auto keys = object.value(u"keys"_q).toObject();
				for (auto k = keys.begin(); k != keys.end(); ++k) {
					const auto entry = k.value().toObject();
					const auto key = Base64(entry.value(u"publicKey"_q).toString());
					const auto sent = Number(entry.value(u"sent"_q).toString(), true);
					const auto control = Number(entry.value(u"control"_q).toString(), true);
					if (!key || !ValidPublicKey(*key) || FingerprintFor(*key) != k.key()
						|| !sent || !control) {
						return false;
					}
					auto record = PeerState::Key();
					record.publicKey = *key;
					record.sent = *sent;
					record.control = *control;
					if (!loadReceipts(entry.value(u"incoming"_q), record.incoming, false)
						|| !loadReceipts(entry.value(u"outgoing"_q), record.outgoing, true)
						|| (!record.outgoing.empty() && record.outgoing.rbegin()->first > *sent)
						|| (*control && !record.incoming.contains(*control))) {
						return false;
					}
					peer.keys.emplace(k.key(), std::move(record));
				}
				if (!peer.keys.contains(peer.active)) {
					return false;
				}
			}
			peers.emplace(PeerId(*id), std::move(peer));
		}
		return version == 2 || save();
	}

	bool save() {
		if (failed || !identity) {
			return false;
		}
		auto identityBytes = QByteArray(kIdentityVersion);
		identityBytes += identity->privateKey;
		identityBytes += identity->publicKey;
		auto state = QJsonObject();
		for (const auto &[peerId, peer] : peers) {
			auto keys = QJsonObject();
			for (const auto &[fingerprint, key] : peer.keys) {
				keys.insert(fingerprint, QJsonObject{
					{ u"publicKey"_q, QString::fromLatin1(key.publicKey.toBase64()) },
					{ u"sent"_q, QString::number(key.sent) },
					{ u"control"_q, QString::number(key.control) },
					{ u"incoming"_q, receiptsJson(key.incoming) },
					{ u"outgoing"_q, receiptsJson(key.outgoing) },
				});
			}
			state.insert(QString::number(peerId.value), QJsonObject{
				{ u"active"_q, peer.active },
				{ u"mode"_q, QJsonObject{
					{ u"requested"_q, peer.mode.requested },
					{ u"ready"_q, peer.mode.ready },
					{ u"keyChanged"_q, peer.mode.keyChanged },
					{ u"challenge"_q, peer.mode.challenge },
					{ u"lastPeerChallenge"_q, peer.mode.lastPeerChallenge },
				} },
				{ u"keys"_q, keys },
			});
		}
		if (!session->local().writeE2eState(EncodeState(QJsonObject{
				{ u"version"_q, 2 },
				{ u"account"_q, QString::number(session->userId().bare) },
				{ u"identity"_q, QString::fromLatin1(identityBytes.toBase64()) },
				{ u"peers"_q, state },
			}))) {
			return false;
		}
		session->local().clearPref(kIdentityPref);
		session->local().clearPref(kPeersPref);
		return true;
	}

};

Chat::Chat(not_null<Main::Session*> session)
: _private(std::make_unique<Private>(session)) {
}

Chat::~Chat() = default;

Chat::Identity *Chat::identity() {
	if (_private->failed) {
		return nullptr;
	}
	if (!_private->identity) {
		const auto key = NewKey();
		if (!key) {
			return nullptr;
		}
		_private->identity = Identity{
			PrivateBytes(key.get()),
			PublicBytes(key.get())};
		if (_private->identity->privateKey.isEmpty()
			|| _private->identity->publicKey.isEmpty()
			|| !_private->save()) {
			_private->identity.reset();
			return nullptr;
		}
	}
	return &*_private->identity;
}

Chat::PeerState *Chat::peer(PeerId peerId) {
	const auto i = _private->peers.find(peerId);
	return (i == _private->peers.end()) ? nullptr : &i->second;
}

const Chat::PeerState *Chat::peer(PeerId peerId) const {
	const auto i = _private->peers.find(peerId);
	return (i == _private->peers.end()) ? nullptr : &i->second;
}

bool Chat::save() {
	return _private->save();
}

QByteArray Chat::publicKey() const {
	const auto local = const_cast<Chat*>(this)->identity();
	return local ? local->publicKey : QByteArray();
}

QString Chat::publicKeyBase64() const {
	return QString::fromLatin1(publicKey().toBase64());
}

QString Chat::fingerprint() const {
	const auto key = publicKey();
	return key.isEmpty() ? QString() : FingerprintFor(key);
}

QString Chat::peerPublicKeyBase64(PeerId peerId) const {
	const auto state = peer(peerId);
	return state
		? QString::fromLatin1(state->keys.at(state->active).publicKey.toBase64())
		: QString();
}

QString Chat::peerFingerprint(PeerId peerId) const {
	const auto state = peer(peerId);
	return state ? state->active : QString();
}

QString Chat::keyExchangeMessage() const {
	const auto key = publicKeyBase64();
	return key.isEmpty()
		? QString()
		: kKeyExchangePrefix + key + u"."_q + fingerprint();
}

QString Chat::stateMessage(PeerId peerId, bool enabled) {
	const auto state = peer(peerId);
	if (!state || state->mode.requested != enabled || state->mode.keyChanged) {
		return {};
	}
	return encryptFrame(
		peerId,
		enabled ? u"ON" : u"OFF",
		enabled ? state->mode.challenge : QString()).value_or(QString());
}

QString Chat::controlNotice(
		PeerId peerId,
		QStringView control,
		bool outgoing,
		bool live) {
	const auto parts = control.split(u'.');
	if (IsKeyExchange(control)) {
		const auto key = (parts.size() == 4) ? Base64(parts[2]) : std::nullopt;
		if (!key || !ValidPublicKey(*key) || FingerprintFor(*key) != parts[3]) {
			return u"Invalid E2E public key message."_q;
		}
		if (outgoing) {
			return u"You sent your E2E public key."_q;
		}
		const auto known = peer(peerId);
		if (known && known->active != parts[3]) {
			if (live && !known->mode.keyChanged) {
				known->mode.keyChanged = true;
				known->mode.ready = false;
				if (!save()) {
					_private->failed = true;
				}
				_private->changes.fire_copy(peerId);
			}
			return u"Your contact's E2E key changed. Verify their fingerprint in Encryption keys before sending encrypted messages.\n\nPublic key:\n"_q
				+ parts[2] + u"\n\nFingerprint:\n"_q + parts[3];
		}
		return u"Your contact sent an E2E public key. Verify the fingerprint through a trusted second channel, then enable encryption.\n\nPublic key:\n"_q
			+ parts[2] + u"\n\nFingerprint:\n"_q + parts[3];
	}
	if (outgoing && parts.size() == 9) {
		if (parts[1] == u"ON") {
			return u"You requested E2E encryption. Waiting for your contact's confirmation."_q;
		} else if (parts[1] == u"OFF") {
			return u"You turned off E2E encryption for new messages."_q;
		} else if (parts[1] == u"ACK") {
			return u"You acknowledged your contact's encryption request."_q;
		}
	}
	return u"Unverified E2E control message."_q;
}

bool Chat::hasPeerKey(PeerId peerId) const {
	return !_private->failed && peer(peerId) != nullptr;
}

bool Chat::enabled(PeerId peerId) const {
	if (_private->failed) {
		return peerIsUser(peerId);
	}
	const auto state = peer(peerId);
	return state && state->mode.requested;
}

bool Chat::ready(PeerId peerId) const {
	const auto state = peer(peerId);
	return !_private->failed && state && state->mode.requested
		&& state->mode.ready && !state->mode.keyChanged;
}

bool Chat::isEncrypted(PeerId peerId, MsgId messageId) const {
	if (!messageId) {
		return false;
	}
	const auto state = peer(peerId);
	if (!state) {
		return false;
	}
	for (const auto &[fingerprint, key] : state->keys) {
		for (const auto receipts : { &key.incoming, &key.outgoing }) {
			for (const auto &[sequence, receipt] : *receipts) {
				if (receipt.messageId == messageId) {
					return true;
				}
			}
		}
	}
	return false;
}

rpl::producer<PeerId> Chat::changes() const {
	return _private->changes.events();
}

bool Chat::setPeerKey(PeerId peerId, const QByteArray &publicKey) {
	if (const auto state = peer(peerId)) {
		return !_private->failed && !state->mode.keyChanged
			&& state->keys.at(state->active).publicKey == publicKey;
	}
	return replacePeerKey(peerId, publicKey);
}

bool Chat::replacePeerKey(PeerId peerId, const QByteArray &publicKey) {
	if (!peerId || !peerIsUser(peerId)
		|| peerToUser(peerId) == _private->session->userId()
		|| !ValidPublicKey(publicKey) || !identity()) {
		return false;
	}
	const auto insertedPeer = _private->peers.try_emplace(peerId).second;
	const auto state = peer(peerId);
	const auto previousActive = state->active;
	const auto previousMode = state->mode;
	const auto fingerprint = FingerprintFor(publicKey);
	if (!insertedPeer && previousActive == fingerprint && !state->mode.keyChanged) {
		return true;
	}
	const auto insertedKey = state->keys.try_emplace(fingerprint).second;
	state->keys.at(fingerprint).publicKey = publicKey;
	state->active = fingerprint;
	state->mode = PeerState::Mode();
	state->mode.requested = previousMode.requested;
	state->mode.challenge = previousMode.requested ? NewChallenge() : QString();
	if ((previousMode.requested && state->mode.challenge.isEmpty()) || !save()) {
		if (insertedPeer) {
			_private->peers.erase(peerId);
		} else {
			state->active = previousActive;
			state->mode = previousMode;
			if (insertedKey) {
				state->keys.erase(fingerprint);
			}
		}
		return false;
	}
	_private->changes.fire_copy(peerId);
	return true;
}

bool Chat::setEnabled(PeerId peerId, bool value) {
	const auto state = peer(peerId);
	if (_private->failed || !state || (value && state->mode.keyChanged)) {
		return false;
	}
	const auto previous = state->mode;
	state->mode.requested = value;
	state->mode.ready = false;
	state->mode.challenge = value ? NewChallenge() : QString();
	state->mode.lastPeerChallenge.clear();
	if ((value && state->mode.challenge.isEmpty()) || !save()) {
		state->mode = previous;
		return false;
	}
	_private->changes.fire_copy(peerId);
	return true;
}

bool Chat::disable(PeerId peerId) {
	return setEnabled(peerId, false);
}

std::optional<QString> Chat::encrypt(PeerId peerId, QStringView plaintext) {
	return ready(peerId) && !plaintext.isEmpty()
		? encryptFrame(peerId, u"MSG", plaintext)
		: std::nullopt;
}

std::optional<QString> Chat::encryptFrame(
		PeerId peerId,
		QStringView kind,
		QStringView plaintext) {
	const auto state = peer(peerId);
	const auto local = identity();
	if (!state || !local || !peerIsUser(peerId) || state->mode.keyChanged) {
		return std::nullopt;
	}
	auto &record = state->keys.at(state->active);
	if (record.sent >= quint64(std::numeric_limits<qint64>::max())) {
		return std::nullopt;
	}
	const auto key = LoadPrivate(local->privateKey);
	if (!key) {
		return std::nullopt;
	}
	const auto symmetric = DeriveKey(
		key.get(),
		local->publicKey,
		record.publicKey,
		_private->session->userId().bare,
		peerToUser(peerId).bare,
		true);
	const auto sequence = record.sent + 1;
	const auto header = kEnvelopePrefix + kind
		+ u"."_q + QString::number(_private->session->userId().bare)
		+ u"."_q + QString::number(peerToUser(peerId).bare)
		+ u"."_q + FingerprintFor(local->publicKey)
		+ u"."_q + state->active
		+ u"."_q + QString::number(sequence);
	auto nonce = QByteArray(kNonceSize, Qt::Uninitialized);
	if (symmetric.isEmpty() || RAND_bytes(
			reinterpret_cast<unsigned char *>(nonce.data()),
			nonce.size()) != 1) {
		return std::nullopt;
	}
	const auto encrypted = AesGcmEncrypt(
		symmetric,
		nonce,
		plaintext.toUtf8(),
		header.toUtf8());
	if (encrypted.isEmpty()) {
		return std::nullopt;
	}
	const auto result = header + u"."_q + QString::fromLatin1(nonce.toBase64())
		+ u"."_q + QString::fromLatin1(encrypted.toBase64());
	record.sent = sequence;
	record.outgoing.emplace(sequence, PeerState::Receipt{ {}, Sha256(result.toUtf8()) });
	if (!save()) {
		record.outgoing.erase(sequence);
		--record.sent;
		return std::nullopt;
	}
	return result;
}

std::optional<ReceivedMessage> Chat::receive(
		PeerId peerId,
		MsgId messageId,
		bool outgoing,
		bool live,
		QStringView envelope) {
	const auto state = peer(peerId);
	if (_private->failed || !state || !peerIsUser(peerId)
		|| messageId.bare <= 0 || !_private->identity) {
		return std::nullopt;
	}
	const auto parts = envelope.split(u'.');
	if (parts.size() != 9 || parts[0] != u"TVE2") {
		return std::nullopt;
	}
	const auto kind = parts[1];
	if (kind != u"MSG" && kind != u"ON" && kind != u"ACK" && kind != u"OFF") {
		return std::nullopt;
	}
	const auto sender = Number(parts[2]);
	const auto recipient = Number(parts[3]);
	const auto sequence = Number(parts[6]);
	const auto localId = _private->session->userId().bare;
	const auto remoteId = peerToUser(peerId).bare;
	const auto &local = *_private->identity;
	if (!sender || !recipient || !sequence
		|| *sender != (outgoing ? localId : remoteId)
		|| *recipient != (outgoing ? remoteId : localId)
		|| parts[outgoing ? 4 : 5] != FingerprintFor(local.publicKey)) {
		return std::nullopt;
	}
	const auto fingerprint = parts[outgoing ? 5 : 4].toString();
	const auto found = state->keys.find(fingerprint);
	if (found == state->keys.end()) {
		return std::nullopt;
	}
	auto &record = found->second;
	const auto nonce = Base64(parts[7]);
	const auto ciphertext = Base64(parts[8]);
	if (!nonce || !ciphertext || nonce->size() != kNonceSize
		|| ciphertext->size() < kTagSize) {
		return std::nullopt;
	}
	const auto key = LoadPrivate(local.privateKey);
	if (!key) {
		return std::nullopt;
	}
	const auto symmetric = DeriveKey(
		key.get(),
		local.publicKey,
		record.publicKey,
		localId,
		remoteId,
		outgoing);
	const auto headerLength = envelope.size() - parts[7].size() - parts[8].size() - 2;
	const auto decoded = AesGcmDecrypt(
		symmetric,
		*nonce,
		*ciphertext,
		envelope.first(headerLength).toUtf8());
	if (!decoded) {
		return std::nullopt;
	}
	auto decoder = QStringDecoder(
		QStringDecoder::Utf8,
		QStringConverter::Flag::ConvertInitialBom);
	const auto plaintext = QString(decoder(*decoded));
	if (decoder.hasError()
		|| (kind == u"ON" && !ValidChallenge(plaintext))
		|| (kind == u"OFF" && !plaintext.isEmpty())
		|| (kind == u"ACK" && (plaintext.size() < 2
			|| plaintext[plaintext.size() - 2] != u':'
			|| (plaintext.back() != u'0' && plaintext.back() != u'1')
			|| !ValidChallenge(QStringView(plaintext).first(plaintext.size() - 2))))) {
		return std::nullopt;
	}
	auto &receipts = outgoing ? record.outgoing : record.incoming;
	const auto digest = Sha256(envelope.toUtf8());
	const auto previous = receipts.find(*sequence);
	const auto fresh = previous == receipts.end();
	if (!fresh && (previous->second.digest != digest
		|| (previous->second.messageId && previous->second.messageId != messageId))) {
		return std::nullopt;
	}
	const auto previousId = fresh ? MsgId() : previous->second.messageId;
	const auto previousMode = state->mode;
	const auto previousControl = record.control;
	const auto previousSent = record.sent;
	const auto applyControl = fresh && live && !outgoing && kind != u"MSG"
		&& fingerprint == state->active && !state->mode.keyChanged
		&& *sequence > record.control;
	auto replyAck = false;
	auto repeatOn = false;
	if (applyControl) {
		record.control = *sequence;
		if (kind == u"ON") {
			replyAck = true;
			repeatOn = state->mode.requested && !state->mode.ready
				&& state->mode.lastPeerChallenge != plaintext;
			state->mode.lastPeerChallenge = plaintext;
		} else if (kind == u"OFF") {
			state->mode.ready = false;
		} else if (state->mode.requested
			&& QStringView(plaintext).first(plaintext.size() - 2) == state->mode.challenge) {
			state->mode.ready = plaintext.back() == u'1';
		}
	}
	if (fresh) {
		receipts.emplace(*sequence, PeerState::Receipt{ messageId, digest });
	} else {
		previous->second.messageId = messageId;
	}
	if (outgoing) {
		record.sent = std::max(record.sent, *sequence);
	}
	if ((fresh || previousId != messageId || previousSent != record.sent) && !save()) {
		_private->failed = true;
		if (fresh) {
			receipts.erase(*sequence);
		} else {
			previous->second.messageId = previousId;
		}
		state->mode = previousMode;
		record.control = previousControl;
		record.sent = previousSent;
		return std::nullopt;
	}
	auto result = ReceivedMessage();
	if (kind == u"MSG") {
		result.text = plaintext;
	} else if (outgoing) {
		result.text = controlNotice(peerId, envelope, true);
	} else if (kind == u"ON") {
		result.text = u"Your contact requested E2E encryption. Enable it here after verifying the fingerprint."_q;
	} else if (kind == u"OFF") {
		result.text = u"Your contact turned off E2E encryption. Your sending mode has not changed."_q;
	} else {
		result.text = plaintext.back() == u'1'
			? u"Your contact confirmed an E2E encryption request."_q
			: u"Your contact has not enabled E2E encryption yet."_q;
	}
	if (replyAck) {
		const auto reply = encryptFrame(
			peerId,
			u"ACK",
			plaintext + (state->mode.requested ? u":1"_q : u":0"_q));
		if (reply) {
			result.replies.push_back(*reply);
		} else {
			result.text += u" Acknowledgment could not be saved; retry the encryption request."_q;
		}
	}
	if (repeatOn) {
		const auto reply = stateMessage(peerId, true);
		if (!reply.isEmpty()) {
			result.replies.push_back(reply);
		}
	}
	if (previousMode != state->mode) {
		_private->changes.fire_copy(peerId);
	}
	return result;
}

bool Chat::IsEnvelope(QStringView text) {
	return (text.startsWith(kEnvelopePrefix) && !IsKeyExchange(text))
		|| text.startsWith(u"TVE1.");
}

bool Chat::IsKeyExchange(QStringView text) {
	return text.startsWith(kKeyExchangePrefix);
}

bool Chat::IsControlMessage(QStringView text) {
	return IsKeyExchange(text) || text.startsWith(u"TVE2.ON.")
		|| text.startsWith(u"TVE2.ACK.") || text.startsWith(u"TVE2.OFF.");
}

int Chat::PlaintextLimit(int envelopeLimit) {
	constexpr auto kEnvelopeOverhead = 256;
	constexpr auto kMaximumEncodedUtf16Size = 4;
	return std::max(1, (envelopeLimit - kEnvelopeOverhead)
		/ kMaximumEncodedUtf16Size);
}

} // namespace E2E
