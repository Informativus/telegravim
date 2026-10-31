/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_transfer.h"

#include "storage/local_transfer_format.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>

namespace Storage::LocalTransfer {
namespace {

using namespace LocalData;
using namespace LocalTransferFormat;

constexpr auto kChunk = 256 * 1024;
constexpr auto kIterations = 600000;
constexpr auto kHeaderSize = 40;
constexpr auto kTagSize = 16;
constexpr auto kSettingsLimit = 64 * 1024;

using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

struct Key {
	std::array<unsigned char, 32> bytes = {};
	~Key() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

struct PasswordWiper {
	QByteArray &value;
	~PasswordWiper() { OPENSSL_cleanse(value.data(), value.size()); }
};

struct Quarantine {
	QTemporaryDir directory;
	Key key;
};

[[nodiscard]] Result Fail(Error code, const QString &message) {
	return { code, message };
}

[[nodiscard]] Result Invalid() {
	return Fail(Error::Invalid, u"Invalid or unsupported transfer package."_q);
}

[[nodiscard]] Result LimitError() {
	return Fail(Error::Invalid,
		u"Transfer limit exceeded: 256 MiB of JSON, 16 MiB per module, "
		"32 GiB of resources, 33 GiB per package and 116512 files (32 MiB manifest)."_q);
}

[[nodiscard]] QJsonObject Unknown(
		QJsonObject object,
		std::initializer_list<QString> known) {
	for (const auto &key : known) {
		object.remove(key);
	}
	return object;
}

[[nodiscard]] bool ReadRetainedMetadata(
		const QJsonObject &object,
		QJsonObject &transfer,
		std::initializer_list<QString> structural) {
	const auto value = object.value(u"retained_transfer"_q);
	if (value.isUndefined()) {
		return true;
	} else if (!value.isObject()) {
		return false;
	}
	const auto retained = value.toObject();
	for (const auto &key : structural) {
		if (retained.contains(key)) {
			return false;
		}
	}
	for (auto i = retained.begin(); i != retained.end(); ++i) {
		transfer.insert(i.key(), i.value());
	}
	return true;
}

[[nodiscard]] Result IoError() {
	return Fail(Error::Io, u"Could not read or write the transfer file."_q);
}

[[nodiscard]] Result AuthenticationError() {
	return Fail(Error::Corrupt, u"Incorrect password or damaged transfer file."_q);
}

[[nodiscard]] bool Cancelled(Cancel cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] Result CancelError() {
	return Fail(Error::Cancelled, u"Transfer cancelled."_q);
}

[[nodiscard]] QByteArray NumberBytes(quint32 value) {
	auto result = QByteArray(4, Qt::Uninitialized);
	qToBigEndian(value, result.data());
	return result;
}

[[nodiscard]] bool IsHash(const QString &value) {
	static const auto pattern = QRegularExpression(u"^[0-9a-f]{64}$"_q);
	return pattern.match(value).hasMatch();
}

[[nodiscard]] bool IsUuid(const QString &value) {
	const auto parsed = QUuid(value);
	return !parsed.isNull()
		&& parsed.toString(QUuid::WithoutBraces) == value;
}

[[nodiscard]] bool ReadSize(const QJsonValue &value, qint64 &size) {
	if (!value.isString()) {
		return false;
	}
	const auto text = value.toString();
	auto ok = false;
	size = text.toLongLong(&ok);
	return ok && size >= 0 && QString::number(size) == text;
}

[[nodiscard]] bool BoundedJson(QByteArrayView bytes) {
	auto depth = 0;
	auto quoted = false;
	auto escaped = false;
	for (const auto ch : bytes) {
		if (quoted) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				quoted = false;
			}
		} else if (ch == '"') {
			quoted = true;
		} else if (ch == '{' || ch == '[') {
			if (++depth > kDepthLimit) {
				return false;
			}
		} else if ((ch == '}' || ch == ']') && --depth < 0) {
			return false;
		}
	}
	return !quoted && !depth;
}

[[nodiscard]] bool ParseJson(const QByteArray &bytes, QJsonObject &result) {
	if (!BoundedJson(bytes)) {
		return false;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return false;
	}
	result = document.object();
	return true;
}

[[nodiscard]] Cipher NewCipher() {
	return Cipher(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
}

[[nodiscard]] bool Initialize(
		EVP_CIPHER_CTX *cipher,
		const Key &key,
		QByteArrayView nonce,
		QByteArrayView aad,
		bool encrypt) {
	auto written = 0;
	return cipher
		&& EVP_CipherInit_ex(cipher, EVP_aes_256_gcm(), nullptr,
			nullptr, nullptr, encrypt ? 1 : 0) == 1
		&& EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_IVLEN,
			nonce.size(), nullptr) == 1
		&& EVP_CipherInit_ex(cipher, nullptr, nullptr, key.bytes.data(),
			reinterpret_cast<const unsigned char*>(nonce.data()), -1) == 1
		&& (aad.empty() || EVP_CipherUpdate(cipher, nullptr, &written,
			reinterpret_cast<const unsigned char*>(aad.data()), aad.size()) == 1);
}

[[nodiscard]] bool Crypt(
		EVP_CIPHER_CTX *cipher,
		QByteArrayView input,
		QByteArray &output) {
	output.resize(input.size() + EVP_MAX_BLOCK_LENGTH);
	auto size = 0;
	if (EVP_CipherUpdate(cipher,
		reinterpret_cast<unsigned char*>(output.data()), &size,
		reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
		return false;
	}
	output.resize(size);
	return true;
}

[[nodiscard]] bool FinishWrite(EVP_CIPHER_CTX *cipher, QFileDevice &file) {
	auto tail = std::array<unsigned char, EVP_MAX_BLOCK_LENGTH>();
	auto size = 0;
	auto tag = QByteArray(kTagSize, Qt::Uninitialized);
	return EVP_CipherFinal_ex(cipher, tail.data(), &size) == 1
		&& size == 0
		&& EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_GET_TAG,
			kTagSize, tag.data()) == 1
		&& file.write(tag) == tag.size();
}

[[nodiscard]] bool FinishRead(EVP_CIPHER_CTX *cipher, QByteArray tag) {
	auto tail = std::array<unsigned char, EVP_MAX_BLOCK_LENGTH>();
	auto size = 0;
	return tag.size() == kTagSize
		&& EVP_CIPHER_CTX_ctrl(cipher, EVP_CTRL_GCM_SET_TAG,
			kTagSize, tag.data()) == 1
		&& EVP_CipherFinal_ex(cipher, tail.data(), &size) == 1
		&& size == 0;
}

[[nodiscard]] bool Derive(
		const QByteArray &password,
		QByteArrayView salt,
		Key &key) {
	return PKCS5_PBKDF2_HMAC(password.data(), password.size(),
		reinterpret_cast<const unsigned char*>(salt.data()), salt.size(),
		kIterations, EVP_sha256(), key.bytes.size(), key.bytes.data()) == 1;
}

[[nodiscard]] Result DecryptPass(
		QFile &file,
		const QByteArray &header,
		const Key &key,
		AssetSink sink,
		QByteArray &digest,
		Cancel cancel,
		ProgressCallback progress,
		int pass) {
	if (!file.seek(kHeaderSize)) {
		return IoError();
	}
	const auto total = file.size();
	auto remaining = total - kHeaderSize - kTagSize;
	auto cipher = NewCipher();
	if (!Initialize(cipher.get(), key, QByteArrayView(header).last(12),
		header, false)) {
		return AuthenticationError();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(header);
	auto plain = QByteArray();
	while (remaining > 0) {
		if (Cancelled(cancel)) {
			return CancelError();
		}
		const auto input = file.read(std::min<qint64>(remaining, kChunk));
		if (input.isEmpty()) {
			return IoError();
		}
		hash.addData(input);
		if (!Crypt(cipher.get(), input, plain)) {
			return AuthenticationError();
		}
		const auto accepted = !sink || sink(plain);
		OPENSSL_cleanse(plain.data(), plain.size());
		if (!accepted) {
			return Invalid();
		}
		remaining -= input.size();
		if (progress) {
			progress({ quint64(pass * total + file.pos()), quint64(2 * total) });
		}
	}
	const auto tag = file.read(kTagSize);
	hash.addData(tag);
	if (!FinishRead(cipher.get(), tag) || !file.atEnd()) {
		return AuthenticationError();
	}
	digest = hash.result();
	return {};
}

[[nodiscard]] AssetReader QuarantineReader(
		std::shared_ptr<Quarantine> quarantine,
		QString path,
		QByteArray nonce,
		QByteArray expectedHash,
		qint64 expectedSize) {
	return [=](AssetSink sink, Cancel cancel) -> Result {
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)
			|| file.size() != expectedSize + kTagSize) {
			return IoError();
		}
		auto cipher = NewCipher();
		if (!Initialize(cipher.get(), quarantine->key, nonce, expectedHash, false)) {
			return AuthenticationError();
		}
		auto remaining = expectedSize;
		auto plain = QByteArray();
		auto hash = QCryptographicHash(QCryptographicHash::Sha256);
		while (remaining > 0) {
			if (Cancelled(cancel)) {
				return CancelError();
			}
			const auto input = file.read(std::min<qint64>(remaining, kChunk));
			if (input.isEmpty() || !Crypt(cipher.get(), input, plain)) {
				return IoError();
			}
			hash.addData(plain);
			const auto accepted = sink(plain);
			OPENSSL_cleanse(plain.data(), plain.size());
			if (!accepted) {
				return IoError();
			}
			remaining -= input.size();
		}
		if (!FinishRead(cipher.get(), file.read(kTagSize))
			|| hash.result().toHex() != expectedHash) {
			return AuthenticationError();
		}
		return {};
	};
}

class Parser final {
public:
	Parser(Inspection &result, Cancel cancel);
	[[nodiscard]] bool consume(QByteArrayView input);
	[[nodiscard]] bool finished() const;
	[[nodiscard]] Result status() const;

private:
	[[nodiscard]] bool manifest();
	[[nodiscard]] bool startEntry();
	[[nodiscard]] bool finishEntry();
	[[nodiscard]] bool fail(Result result = Invalid());

	Inspection &_result;
	Cancel _cancel = nullptr;
	Result _status;
	QByteArray _buffer;
	qint64 _manifestSize = -1;
	std::vector<Entry> _entries;
	QMap<QString, std::pair<QString, QString>> _jsonTargets;
	std::shared_ptr<Quarantine> _quarantine;
	std::unique_ptr<QFile> _asset;
	Cipher _cipher = NewCipher();
	QByteArray _nonce;
	QCryptographicHash _hash = QCryptographicHash(QCryptographicHash::Sha256);
	qint64 _remaining = 0;
	int _index = -1;
	bool _loaded = false;

};

Parser::Parser(Inspection &result, Cancel cancel)
: _result(result)
, _cancel(cancel) {
}

bool Parser::fail(Result result) {
	_status = std::move(result);
	return false;
}

Result Parser::status() const {
	return _status;
}

bool Parser::finished() const {
	return _loaded && _index == _entries.size() && !_remaining;
}

bool Parser::manifest() {
	auto object = QJsonObject();
	if (!ParseJson(_buffer, object)
		|| object.value(u"format"_q) != u"telegravim-transfer"_q
		|| object.value(u"version"_q).toInt() != 1
		|| !IsUuid(object.value(u"package_id"_q).toString())
		|| !IsUuid(object.value(u"device_id"_q).toString())
		|| !IsUuid(object.value(u"generation"_q).toString())
		|| !object.value(u"extra"_q).isObject()
		|| !object.value(u"profiles"_q).isArray()
		|| !object.value(u"assets"_q).isArray()
		|| !object.value(u"files"_q).isArray()
		|| !ReadRetainedMetadata(object, _result.snapshot.transfer, {
			u"manifest"_q,
		})) {
		return fail();
	}
	_result.packageId = object.value(u"package_id"_q).toString();
	_result.snapshot.deviceId = object.value(u"device_id"_q).toString();
	_result.snapshot.generation = object.value(u"generation"_q).toString();
	_result.snapshot.extra = object.value(u"extra"_q).toObject();
	_result.snapshot.transfer.insert(u"manifest"_q, Unknown(object, {
		u"format"_q, u"version"_q, u"package_id"_q, u"created_at"_q,
		u"device_id"_q, u"generation"_q, u"extra"_q, u"profiles"_q,
		u"assets"_q, u"files"_q, u"retained_transfer"_q,
	}));
	auto expected = QSet<QString>();
	for (const auto value : object.value(u"profiles"_q).toArray()) {
		if (Cancelled(_cancel)) {
			return fail(CancelError());
		}
		const auto profile = value.toObject();
		const auto id = profile.value(u"profile_id"_q).toString();
		const auto path = profile.value(u"account"_q).toString();
		const auto prefix = u"profiles/"_q + id + '/';
		if (!IsUuid(id) || path != prefix + u"account.json"_q
			|| expected.contains(path) || !profile.value(u"modules"_q).isArray()
			|| _result.snapshot.profiles.size() >= Limits().maxProfiles) {
			return fail();
		}
		expected.insert(path);
		_jsonTargets.insert(path, { id, QString() });
		_result.snapshot.profiles.insert(id, Profile{ .id = id });
		auto &savedProfile = _result.snapshot.profiles[id];
		savedProfile.transfer.insert(u"profile"_q, Unknown(profile, {
			u"profile_id"_q, u"account"_q, u"modules"_q,
		}));
		auto moduleMetadata = QJsonObject();
		for (const auto value : profile.value(u"modules"_q).toArray()) {
			if (Cancelled(_cancel)) {
				return fail(CancelError());
			}
			const auto module = value.toObject();
			const auto moduleId = module.value(u"module_id"_q).toString();
			const auto path = module.value(u"path"_q).toString();
			if (!ValidModuleId(moduleId)
				|| path != prefix + u"modules/"_q + moduleId + u".json"_q
				|| expected.contains(path)) {
				return fail();
			}
			expected.insert(path);
			_jsonTargets.insert(path, { id, moduleId });
			moduleMetadata.insert(moduleId, Unknown(module, {
				u"module_id"_q, u"path"_q,
			}));
		}
		savedProfile.transfer.insert(u"modules"_q, moduleMetadata);
	}
	for (const auto value : object.value(u"assets"_q).toArray()) {
		if (Cancelled(_cancel)) {
			return fail(CancelError());
		}
		const auto asset = value.toObject();
		const auto hash = asset.value(u"sha256"_q).toString();
		const auto path = u"assets/"_q + hash;
		auto size = qint64(0);
		auto transfer = QJsonObject();
		if (!IsHash(hash) || expected.contains(path)
			|| !asset.value(u"extra"_q).isObject()
			|| !ReadSize(asset.value(u"size"_q), size) || size > kAssetLimit
			|| !ReadRetainedMetadata(asset, transfer, {
				u"registry"_q, u"file"_q,
			})) {
			return fail();
		}
		expected.insert(path);
		_result.assetBytes += size;
		if (_result.assetBytes > kAssetLimit) {
			return fail(LimitError());
		}
		transfer.insert(u"registry"_q, Unknown(asset, {
			u"sha256"_q, u"size"_q, u"extra"_q, u"retained_transfer"_q,
		}));
		_result.snapshot.assets.insert(hash, Asset{
			.sha256 = hash,
			.size = size,
			.extra = asset.value(u"extra"_q).toObject(),
			.transfer = std::move(transfer),
		});
	}
	auto jsonBytes = qint64(0);
	auto total = qint64(0);
	for (const auto value : object.value(u"files"_q).toArray()) {
		if (Cancelled(_cancel)) {
			return fail(CancelError());
		}
		const auto entry = value.toObject();
		const auto path = entry.value(u"path"_q).toString();
		const auto hash = entry.value(u"sha256"_q).toString();
		auto size = qint64(0);
		if (!expected.remove(path) || !IsHash(hash)
			|| !ReadSize(entry.value(u"size"_q), size) || size > kAssetLimit) {
			return fail();
		}
		if (_jsonTargets.contains(path)) {
			jsonBytes += size;
			if (size > kJsonLimit || jsonBytes > kAllJsonLimit) {
				return fail(LimitError());
			}
			const auto target = _jsonTargets[path];
			auto &profile = _result.snapshot.profiles[target.first];
			auto files = profile.transfer.value(u"files"_q).toObject();
			const auto extra = Unknown(entry, { u"path"_q, u"size"_q, u"sha256"_q });
			if (target.second.isEmpty()) {
				files.insert(u"account"_q, extra);
			} else {
				auto modules = files.value(u"modules"_q).toObject();
				modules.insert(target.second, extra);
				files.insert(u"modules"_q, modules);
			}
			profile.transfer.insert(u"files"_q, files);
		} else {
			const auto asset = _result.snapshot.assets.constFind(hash);
			if (asset == _result.snapshot.assets.cend()
				|| path != u"assets/"_q + hash || asset->size != size) {
				return fail();
			}
			_result.snapshot.assets[hash].transfer.insert(u"file"_q,
				Unknown(entry, { u"path"_q, u"size"_q, u"sha256"_q }));
		}
		total += size;
		if (total > kPackageLimit || _entries.size() >= kEntryLimit) {
			return fail(LimitError());
		}
		_entries.push_back({ .path = path, .size = size, .hash = hash.toLatin1() });
	}
	if (!expected.isEmpty()) {
		return fail();
	}
	_buffer.clear();
	_loaded = true;
	return startEntry();
}

bool Parser::startEntry() {
	++_index;
	if (_index == _entries.size()) {
		return true;
	}
	const auto &entry = _entries[_index];
	_remaining = entry.size;
	_hash.reset();
	_buffer.clear();
	if (!_jsonTargets.contains(entry.path)) {
		if (!_quarantine) {
			_quarantine = std::make_shared<Quarantine>();
			if (!_quarantine->directory.isValid()
				|| RAND_bytes(_quarantine->key.bytes.data(), 32) != 1) {
				return fail(IoError());
			}
		}
		_nonce = QByteArray(12, Qt::Uninitialized);
		if (RAND_bytes(reinterpret_cast<unsigned char*>(_nonce.data()), 12) != 1
			|| !Initialize(_cipher.get(), _quarantine->key,
				_nonce, entry.hash, true)) {
			return fail(IoError());
		}
		const auto path = _quarantine->directory.filePath(QString::number(_index));
		_asset = std::make_unique<QFile>(path);
		if (!_asset->open(QIODevice::WriteOnly | QIODevice::NewOnly)
			|| !_asset->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
			return fail(IoError());
		}
	}
	return true;
}

bool Parser::finishEntry() {
	const auto &entry = _entries[_index];
	if (_hash.result().toHex() != entry.hash) {
		return fail(AuthenticationError());
	}
	if (_asset) {
		if (!FinishWrite(_cipher.get(), *_asset) || !_asset->flush()) {
			return fail(IoError());
		}
		auto &asset = _result.snapshot.assets[QString::fromLatin1(entry.hash)];
		asset.read = QuarantineReader(_quarantine, _asset->fileName(),
			_nonce, entry.hash, entry.size);
		_asset.reset();
	} else {
		auto object = QJsonObject();
		if (!ParseJson(_buffer, object)) {
			return fail();
		}
		const auto &target = _jsonTargets[entry.path];
		auto &profile = _result.snapshot.profiles[target.first];
		if (target.second.isEmpty()) {
			if (object.value(u"profile_id"_q) != profile.id
				|| !object.value(u"extra"_q).isObject()
				|| !ReadRetainedMetadata(object, profile.transfer, {
					u"account"_q, u"profile"_q, u"modules"_q, u"files"_q,
				})) {
				return fail();
			}
			profile.account = {
				object.value(u"environment"_q).toString(),
				object.value(u"user_id"_q).toString(),
			};
			profile.extra = object.value(u"extra"_q).toObject();
			profile.transfer.insert(u"account"_q, Unknown(object, {
				u"profile_id"_q, u"environment"_q, u"user_id"_q, u"extra"_q,
				u"retained_transfer"_q,
			}));
		} else {
			if (object.value(u"module_id"_q) != target.second) {
				return fail();
			}
			profile.modules.insert(target.second, std::move(object));
		}
	}
	return startEntry();
}

bool Parser::consume(QByteArrayView input) {
	while (!input.empty() || (_loaded && !finished() && !_remaining)) {
		if (Cancelled(_cancel)) {
			return fail(CancelError());
		}
		if (!_loaded) {
			const auto wanted = (_manifestSize < 0) ? 12 : _manifestSize;
			const auto take = std::min<qsizetype>(wanted - _buffer.size(), input.size());
			_buffer.append(input.data(), take);
			input = input.sliced(take);
			if (_buffer.size() < wanted) {
				return true;
			}
			if (_manifestSize < 0) {
				if (!_buffer.startsWith("TGVPKG01")) {
					return fail();
				}
				_manifestSize = qFromBigEndian<quint32>(_buffer.constData() + 8);
				if (_manifestSize < 2 || _manifestSize > kManifestLimit) {
					return fail();
				}
				_buffer.clear();
			} else if (!manifest()) {
				return false;
			}
		} else if (finished()) {
			return fail();
		} else if (!_remaining) {
			if (!finishEntry()) {
				return false;
			}
		} else {
			const auto take = std::min<qint64>(_remaining, input.size());
			const auto part = input.first(take);
			_hash.addData(part);
			if (_asset) {
				auto encrypted = QByteArray();
				if (!Crypt(_cipher.get(), part, encrypted)
					|| _asset->write(encrypted) != encrypted.size()) {
					return fail(IoError());
				}
			} else {
				_buffer.append(part.data(), part.size());
			}
			_remaining -= take;
			input = input.sliced(take);
		}
	}
	return true;
}

} // namespace

LocalData::Result Export(
		const LocalData::Snapshot &snapshot,
		const QString &path,
		QByteArray password,
		const std::atomic<bool> *cancel,
		ProgressCallback progress,
		PasswordRequirement passwordRequirement) {
	const auto wiper = PasswordWiper{ password };
	const auto minimum = (passwordRequirement == PasswordRequirement::NonEmpty)
		? 1
		: 12;
	if (password.size() < minimum || password.size() > 1024) {
		return Fail(Error::Invalid, u"The export password is empty, too short or too long."_q);
	}
	if (const auto checked = ValidateSnapshot(snapshot, Limits(), cancel)
		; !checked) {
		return checked;
	}
	auto prepared = LocalTransferFormat::Manifest();
	if (const auto built = LocalTransferFormat::Prepare(snapshot, prepared, true, cancel)
		; !built) {
		return built;
	}
	const auto &entries = prepared.entries;
	const auto &manifest = prepared.json;
	const auto total = prepared.payloadBytes;
	auto header = QByteArray("TGVXFR01") + NumberBytes(kIterations)
		+ QByteArray(28, Qt::Uninitialized);
	auto key = Key();
	if (RAND_bytes(reinterpret_cast<unsigned char*>(header.data() + 12), 28) != 1
		|| !Derive(password, QByteArrayView(header).sliced(12, 16), key)) {
		return IoError();
	}
	if (Cancelled(cancel)) {
		return CancelError();
	}
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)
		|| !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
		|| file.write(header) != header.size()) {
		return IoError();
	}
	auto cipher = NewCipher();
	if (!Initialize(cipher.get(), key, QByteArrayView(header).last(12), header, true)) {
		return IoError();
	}
	auto completed = qint64(0);
	const auto write = [&](QByteArrayView plain) {
		while (!plain.empty()) {
			if (Cancelled(cancel)) {
				return false;
			}
			const auto part = plain.first(std::min<qsizetype>(plain.size(), kChunk));
			auto output = QByteArray();
			if (!Crypt(cipher.get(), part, output)
				|| file.write(output) != output.size()) {
				return false;
			}
			completed += part.size();
			if (progress) {
				progress({ quint64(completed), quint64(total) });
			}
			plain = plain.sliced(part.size());
		}
		return true;
	};
	if (!write(QByteArray("TGVPKG01") + NumberBytes(manifest.size()))
		|| !write(manifest)) {
		return Cancelled(cancel) ? CancelError() : IoError();
	}
	for (const auto &entry : entries) {
		auto hash = QCryptographicHash(QCryptographicHash::Sha256);
		auto bytes = qint64(0);
		const auto sink = [&](QByteArrayView part) {
			if (part.size() > entry.size - bytes) {
				return false;
			}
			bytes += part.size();
			hash.addData(part);
			return write(part);
		};
		const auto result = entry.read
			? entry.read(sink, cancel)
			: (sink(entry.json) ? Result() : IoError());
		if (!result) {
			return Cancelled(cancel) ? CancelError() : result;
		}
		if (bytes != entry.size || hash.result().toHex() != entry.hash) {
			return Fail(Error::Corrupt, u"A local resource changed during export."_q);
		}
	}
	if (Cancelled(cancel)) {
		return CancelError();
	}
	if (!FinishWrite(cipher.get(), file) || !file.commit()) {
		return IoError();
	}
	return {};
}

LocalData::Result ExportSettings(
		const QByteArray &module,
		QByteArray password,
		QByteArray &result) {
	const auto wiper = PasswordWiper{ password };
	if (password.isEmpty() || password.size() > 1024) {
		return Fail(Error::Invalid, u"The export password is empty or too long."_q);
	} else if (module.isEmpty() || module.size() > kSettingsLimit) {
		return Invalid();
	}
	auto header = QByteArray("TGVSET01") + NumberBytes(kIterations)
		+ QByteArray(28, Qt::Uninitialized);
	auto key = Key();
	if (RAND_bytes(reinterpret_cast<unsigned char*>(header.data() + 12), 28) != 1
		|| !Derive(password, QByteArrayView(header).sliced(12, 16), key)) {
		return IoError();
	}
	auto cipher = NewCipher();
	auto encrypted = QByteArray();
	auto tail = std::array<unsigned char, EVP_MAX_BLOCK_LENGTH>();
	auto size = 0;
	auto tag = QByteArray(kTagSize, Qt::Uninitialized);
	if (!Initialize(cipher.get(), key, QByteArrayView(header).last(12), header, true)
		|| !Crypt(cipher.get(), module, encrypted)
		|| EVP_CipherFinal_ex(cipher.get(), tail.data(), &size) != 1
		|| size != 0
		|| EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG,
			kTagSize, tag.data()) != 1) {
		return IoError();
	}
	result = header + encrypted + tag;
	return {};
}

LocalData::Result Inspect(
		const QString &path,
		QByteArray password,
		Inspection &result,
		const std::atomic<bool> *cancel,
		ProgressCallback progress) {
	const auto wiper = PasswordWiper{ password };
	if (password.isEmpty() || password.size() > 1024) {
		return AuthenticationError();
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return IoError();
	}
	const auto size = file.size();
	if (size < kHeaderSize + kTagSize + 12 || size > kPackageLimit + 56) {
		return LimitError();
	}
	const auto header = file.read(kHeaderSize);
	if (header.size() != kHeaderSize || !header.startsWith("TGVXFR01")
		|| qFromBigEndian<quint32>(header.constData() + 8) != kIterations) {
		return Invalid();
	}
	auto key = Key();
	if (!Derive(password, QByteArrayView(header).sliced(12, 16), key)) {
		return AuthenticationError();
	}
	auto firstHash = QByteArray();
	if (const auto checked = DecryptPass(file, header, key, {},
		firstHash, cancel, progress, 0); !checked) {
		return checked;
	}
	auto inspected = Inspection{ .packageBytes = quint64(size) };
	auto parser = Parser(inspected, cancel);
	auto secondHash = QByteArray();
	const auto parsed = DecryptPass(file, header, key,
		[&](QByteArrayView bytes) { return parser.consume(bytes); },
		secondHash, cancel, progress, 1);
	if (!parser.status()) {
		return parser.status();
	} else if (!parsed) {
		return parsed;
	} else if (!parser.finished() || firstHash != secondHash || file.size() != size) {
		return AuthenticationError();
	}
	if (const auto checked = ValidateSnapshot(inspected.snapshot, Limits(), cancel)
		; !checked) {
		return checked;
	}
	result = std::move(inspected);
	return {};
}

} // namespace Storage::LocalTransfer
