/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/local_data_store.h"
#include "storage/local_transfer_format.h"
#include "storage/local_pinned_items_format.h"

#include "mtproto/mtproto_auth_key.h"
#include <crl/crl_on_main.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtCore/QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#else // Q_OS_WIN
#include <sys/stat.h>
#endif // Q_OS_WIN

namespace Storage::LocalData {
namespace {

constexpr auto kAssetChunk = 256 * 1024;
constexpr auto kMaxCounter = 9007199254740991.;
constexpr auto kOwnerPermissions = QFileDevice::ReadOwner
	| QFileDevice::WriteOwner;

struct FileStamp {
	qint64 size = 0;
	quint64 device = 0;
	quint64 identity = 0;
	std::array<qint64, 4> times = {};
	friend bool operator==(const FileStamp &, const FileStamp &) = default;
};

[[nodiscard]] std::optional<FileStamp> FileIdentity(const QString &path) {
#ifdef Q_OS_WIN
	const auto handle = CreateFileW(
		reinterpret_cast<const wchar_t*>(path.utf16()),
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		return std::nullopt;
	}
	auto identity = BY_HANDLE_FILE_INFORMATION();
	auto basic = FILE_BASIC_INFO();
	const auto valid = GetFileInformationByHandle(handle, &identity)
		&& GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
		&& !(identity.dwFileAttributes
			& (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
	CloseHandle(handle);
	if (!valid) {
		return std::nullopt;
	}
	return FileStamp{
		qint64((quint64(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow),
		identity.dwVolumeSerialNumber,
		(quint64(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow,
		{ basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart },
	};
#else // Q_OS_WIN
	struct stat info = {};
	if (::lstat(QFile::encodeName(path).constData(), &info) != 0
		|| !S_ISREG(info.st_mode)) {
		return std::nullopt;
	}
#ifdef Q_OS_MAC
	const auto modified = info.st_mtimespec;
	const auto changed = info.st_ctimespec;
#else // Q_OS_MAC
	const auto modified = info.st_mtim;
	const auto changed = info.st_ctim;
#endif // Q_OS_MAC
	return FileStamp{
		qint64(info.st_size),
		quint64(info.st_dev),
		quint64(info.st_ino),
		{ qint64(modified.tv_sec), qint64(modified.tv_nsec),
			qint64(changed.tv_sec), qint64(changed.tv_nsec) },
	};
#endif // Q_OS_WIN
}

[[nodiscard]] bool HasStoredFiles(const QString &path) {
	for (const auto &directory : { u"/blobs"_q, u"/assets"_q }) {
		auto entries = QDirIterator(
			path + directory,
			QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
		if (entries.hasNext()) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] Result Failure(Error code, const QString &message) {
	return { code, message };
}

[[nodiscard]] bool Cancelled(Cancel cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] Result Cancellation() {
	return Failure(Error::Cancelled, u"Operation cancelled."_q);
}

[[nodiscard]] QByteArray Json(const QJsonObject &value) {
	return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QString Digest(QByteArrayView bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(
		bytes,
		QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool ValidHash(const QString &value) {
	return value.size() == 64
		&& std::all_of(value.begin(), value.end(), [](QChar ch) {
			return (ch >= u'0' && ch <= u'9')
				|| (ch >= u'a' && ch <= u'f');
		});
}

[[nodiscard]] bool ValidId(const QString &value) {
	const auto uuid = QUuid(value);
	return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

[[nodiscard]] bool Decimal(const QString &value) {
	return !value.isEmpty()
		&& value.size() <= 20
		&& value.front() >= u'1'
		&& value.front() <= u'9'
		&& std::all_of(value.begin(), value.end(), [](QChar ch) {
			return ch >= u'0' && ch <= u'9';
		});
}

[[nodiscard]] bool ModuleId(const QString &value) {
	return !value.isEmpty()
		&& value.size() <= 96
		&& std::all_of(value.begin(), value.end(), [](QChar ch) {
			return (ch >= u'a' && ch <= u'z')
				|| (ch >= u'0' && ch <= u'9')
				|| ch == u'_';
		});
}

[[nodiscard]] QString AccountId(const Account &account) {
	return account.environment + u":"_q + account.userId;
}

[[nodiscard]] QJsonObject AccountJson(const Account &account) {
	return {
		{ u"environment"_q, account.environment },
		{ u"user_id"_q, account.userId },
	};
}

[[nodiscard]] Account ReadAccount(const QJsonObject &account) {
	return {
		account.value(u"environment"_q).toString(),
		account.value(u"user_id"_q).toString(),
	};
}

[[nodiscard]] bool BoundedJson(const QJsonValue &value, int remaining) {
	if (remaining < 0) {
		return false;
	} else if (value.isArray()) {
		for (const auto &entry : value.toArray()) {
			if (!BoundedJson(entry, remaining - 1)) {
				return false;
			}
		}
	} else if (value.isObject()) {
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (!BoundedJson(i.value(), remaining - 1)) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool ValidVector(const QJsonValue &value) {
	if (!value.isObject() || value.toObject().isEmpty()
		|| value.toObject().size() > 256) {
		return false;
	}
	const auto object = value.toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		const auto counter = i.value().toDouble(-1.);
		if (!ValidId(i.key()) || counter < 1. || counter > kMaxCounter
			|| std::floor(counter) != counter) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QJsonObject JoinedVector(
		QJsonObject first,
		const QJsonObject &second) {
	for (auto i = second.begin(); i != second.end(); ++i) {
		first.insert(i.key(), std::max(
			first.value(i.key()).toDouble(),
			i.value().toDouble()));
	}
	return first;
}

[[nodiscard]] Result Advance(QJsonObject &record, const QString &device) {
	auto vector = record.value(u"version_vector"_q).toObject();
	const auto counter = vector.value(device).toDouble();
	if (counter >= kMaxCounter || vector.size() > 256
		|| (vector.size() == 256 && !vector.contains(device))) {
		return Failure(Error::Invalid, u"Record version limit exceeded."_q);
	}
	vector.insert(device, counter + 1.);
	record.insert(u"version_vector"_q, vector);
	return {};
}

enum class Ordering { Equal, Before, After, Concurrent };

[[nodiscard]] Ordering Compare(
		const QJsonObject &local,
		const QJsonObject &incoming) {
	const auto first = local.value(u"version_vector"_q).toObject();
	const auto second = incoming.value(u"version_vector"_q).toObject();
	auto greater = false;
	auto less = false;
	for (auto i = first.begin(); i != first.end(); ++i) {
		greater |= i.value().toDouble() > second.value(i.key()).toDouble();
		less |= i.value().toDouble() < second.value(i.key()).toDouble();
	}
	for (auto i = second.begin(); i != second.end(); ++i) {
		less |= i.value().toDouble() > first.value(i.key()).toDouble();
	}
	return greater && less ? Ordering::Concurrent
		: greater ? Ordering::After
		: less ? Ordering::Before
		: Ordering::Equal;
}

[[nodiscard]] QMap<QString, QJsonObject> Records(const QJsonObject &module) {
	auto result = QMap<QString, QJsonObject>();
	for (const auto &value : module.value(u"records"_q).toArray()) {
		const auto record = value.toObject();
		result.insert(record.value(u"id"_q).toString(), record);
	}
	return result;
}

void SetRecords(QJsonObject &module, const QMap<QString, QJsonObject> &records) {
	auto values = QJsonArray();
	for (const auto &record : records) {
		values.push_back(record);
	}
	module.insert(u"records"_q, values);
}

[[nodiscard]] Result ValidateTags(
		const QJsonObject &module,
		const Account &account,
		const Limits &limits,
		Cancel cancel) {
	const auto values = module.value(u"records"_q);
	if (!values.isArray() || values.toArray().size() > limits.maxRecords) {
		return Failure(Error::Invalid, u"Invalid module records."_q);
	}
	auto seen = QSet<QString>();
	for (const auto &value : values.toArray()) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto record = value.toObject();
		const auto id = record.value(u"id"_q).toString();
		if (!ValidId(id) || seen.contains(id)
			|| !record.value(u"deleted"_q).isBool()
			|| (record.contains(u"kind"_q) && !record.value(u"kind"_q).isString())
			|| !ValidVector(record.value(u"version_vector"_q))
			|| Json(record).size() > limits.maxRecordBytes) {
			return Failure(Error::Invalid, u"Invalid record identity or version."_q);
		}
		seen.insert(id);
		if (module.value(u"module_id"_q).toString() != u"saved_tags"_q
			|| record.value(u"deleted"_q).toBool()) {
			continue;
		}
		const auto kind = record.value(u"kind"_q).toString();
		if (kind == u"tag"_q) {
			const auto title = record.value(u"title"_q);
			const auto emoji = record.value(u"emoji"_q);
			const auto native = record.value(u"native_reaction"_q);
			const auto reaction = native.toObject();
			const auto reactionKind = reaction.value(u"kind"_q).toString();
			const auto nativeEmoji = reaction.value(u"emoji"_q);
			const auto nativeValid = native.isObject()
				&& ((reactionKind == u"custom"_q
					&& Decimal(reaction.value(u"document_id"_q).toString())
					&& reaction.value(u"document_id"_q).toString().toULongLong() > 0)
					|| (reactionKind == u"emoji"_q && nativeEmoji.isString()
						&& !nativeEmoji.toString().isEmpty()
						&& nativeEmoji.toString() != u"*"_q
						&& nativeEmoji.toString().size() <= 32));
			const auto position = record.value(u"position"_q).toDouble(-1.);
			if (!title.isString()
				|| (title.toString().trimmed().isEmpty() && !nativeValid)
				|| (!native.isUndefined() && !nativeValid)
				|| title.toString().size() > 100 || !emoji.isString()
				|| emoji.toString().size() > 32 || position < 0.
				|| position > 2147483647. || std::floor(position) != position) {
				return Failure(Error::Invalid, u"Invalid saved tag."_q);
			}
		} else if (kind == u"binding"_q) {
			const auto message = record.value(u"message"_q).toObject();
			const auto messageId = message.value(u"message_id"_q).toString();
			const auto text = record.value(u"source_text"_q);
			const auto hash = record.value(u"source_hash"_q);
			const auto reactionPosition = record.value(u"reaction_position"_q);
			const auto position = reactionPosition.toDouble(-1.);
			const auto saved = record.value(u"saved_peer"_q);
			const auto savedType = saved.toObject().value(u"peer_type"_q).toString();
			if (!ValidId(record.value(u"tag_id"_q).toString())
				|| message.value(u"peer_type"_q).toString() != u"user"_q
				|| message.value(u"peer_id"_q).toString() != account.userId
				|| !Decimal(messageId) || messageId.size() > 10
				|| messageId.toULongLong() > 2147483647ULL
				|| (!text.isUndefined() && (!text.isString()
					|| text.toString().size() > 32768))
				|| (!hash.isUndefined() && !ValidHash(hash.toString()))
				|| (!reactionPosition.isUndefined() && (position < 0.
					|| position > 2147483647. || std::floor(position) != position))
				|| (!saved.isUndefined() && (!saved.isObject()
					|| (savedType != u"user" && savedType != u"chat"
						&& savedType != u"channel")
					|| !Decimal(saved.toObject().value(u"peer_id"_q).toString())
					|| saved.toObject().value(u"peer_id"_q).toString().toULongLong() == 0
					|| saved.toObject().value(u"peer_id"_q).toString().toULongLong()
						> 0xFFFFFFFFFFFFULL))) {
				return Failure(Error::Invalid, u"Invalid saved-message binding."_q);
			}
		} else if (kind == u"filter"_q) {
			const auto tags = record.value(u"tags"_q);
			const auto text = record.value(u"text"_q);
			if (!tags.isArray() || !text.isString()
				|| text.toString().size() > 4096) {
				return Failure(Error::Invalid, u"Invalid saved-message filter."_q);
			}
			for (const auto &tag : tags.toArray()) {
				if (!ValidId(tag.toString())) {
					return Failure(Error::Invalid, u"Invalid filter tag identity."_q);
				}
			}
		}
	}
	return {};
}

[[nodiscard]] std::mutex &RegistryMutex() {
	static auto result = std::mutex();
	return result;
}

[[nodiscard]] QMap<QString, ModuleDefinition> &Registry() {
	static auto result = QMap<QString, ModuleDefinition>{
		{ u"pinned_items"_q, ModuleDefinition{
			u"pinned_items"_q,
			1,
			PinnedItems::Validate,
			[](const QJsonObject &) { return QStringList(); },
		} },
		{ u"saved_tags"_q, ModuleDefinition{
			u"saved_tags"_q,
			2,
			[](const QJsonObject &, const Account &) { return Result(); },
			[](const QJsonObject &) { return QStringList(); },
		} },
	};
	return result;
}

[[nodiscard]] std::optional<ModuleDefinition> Definition(
		const QString &id,
		int version) {
	const auto lock = std::lock_guard(RegistryMutex());
	const auto i = Registry().constFind(id);
	return i != Registry().cend() && (version < 0 || i->schemaVersion == version
		|| (id == u"saved_tags"_q && version == 1 && i->schemaVersion == 2))
		? std::optional<ModuleDefinition>(*i) : std::nullopt;
}

[[nodiscard]] Result Crypt(
		bool encrypt,
		const QByteArray &input,
		QByteArray &output,
		const std::shared_ptr<MTP::AuthKey> &localKey,
		QByteArrayView context,
		qint64 limit) {
	if (!localKey || input.size() > limit + (encrypt ? 0 : 36)
		|| (!encrypt && (input.size() < 36 || !input.startsWith("TGLDGCM1")))) {
		return Failure(Error::Corrupt, u"Invalid encrypted data envelope."_q);
	}
	auto derived = std::array<unsigned char, 32>();
	auto length = derived.size();
	const auto source = localKey->data();
	const auto salt = QByteArray("telegravim-local-data-v1");
	const auto info = QByteArray("encrypted-storage");
	auto derivation = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
		EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
	const auto keyReady = derivation
		&& EVP_PKEY_derive_init(derivation.get()) == 1
		&& EVP_PKEY_CTX_set_hkdf_md(derivation.get(), EVP_sha256()) == 1
		&& EVP_PKEY_CTX_set1_hkdf_salt(derivation.get(),
			reinterpret_cast<const unsigned char*>(salt.data()), salt.size()) == 1
		&& EVP_PKEY_CTX_set1_hkdf_key(derivation.get(),
			reinterpret_cast<const unsigned char*>(source.data()), source.size()) == 1
		&& EVP_PKEY_CTX_add1_hkdf_info(derivation.get(),
			reinterpret_cast<const unsigned char*>(info.data()), info.size()) == 1
		&& EVP_PKEY_derive(derivation.get(), derived.data(), &length) == 1
		&& length == derived.size();
	if (!keyReady) {
		OPENSSL_cleanse(derived.data(), derived.size());
		return Failure(Error::Io, u"Local encryption key derivation failed."_q);
	}
	auto header = encrypt ? QByteArray("TGLDGCM1") + QByteArray(12, '\0')
		: input.first(20);
	if (encrypt && RAND_bytes(
		reinterpret_cast<unsigned char*>(header.data() + 8), 12) != 1) {
		OPENSSL_cleanse(derived.data(), derived.size());
		return Failure(Error::Io, u"Secure randomness is unavailable."_q);
	}
	auto cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
		EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
	auto ok = cipher
		&& EVP_CipherInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
			derived.data(), reinterpret_cast<const unsigned char*>(header.data() + 8),
			encrypt ? 1 : 0) == 1;
	OPENSSL_cleanse(derived.data(), derived.size());
	auto written = 0;
	ok = ok && EVP_CipherUpdate(cipher.get(), nullptr, &written,
		reinterpret_cast<const unsigned char*>(header.data()), header.size()) == 1
		&& EVP_CipherUpdate(cipher.get(), nullptr, &written,
			reinterpret_cast<const unsigned char*>(context.data()), context.size()) == 1;
	const auto bodySize = encrypt ? input.size() : input.size() - 36;
	auto body = QByteArray(bodySize + 16, Qt::Uninitialized);
	ok = ok && EVP_CipherUpdate(cipher.get(),
		reinterpret_cast<unsigned char*>(body.data()), &written,
		reinterpret_cast<const unsigned char*>(input.data() + (encrypt ? 0 : 20)),
		bodySize) == 1;
	auto total = written;
	auto tag = encrypt ? QByteArray(16, '\0') : input.last(16);
	if (!encrypt) {
		ok = ok && EVP_CIPHER_CTX_ctrl(cipher.get(),
			EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) == 1;
	}
	ok = ok && EVP_CipherFinal_ex(cipher.get(),
		reinterpret_cast<unsigned char*>(body.data() + total), &written) == 1;
	total += written;
	if (encrypt) {
		ok = ok && EVP_CIPHER_CTX_ctrl(cipher.get(),
			EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
	}
	if (!ok || total != bodySize) {
		OPENSSL_cleanse(body.data(), body.size());
		return Failure(Error::Corrupt, u"Local data authentication failed."_q);
	}
	body.resize(total);
	output = encrypt ? header + body + tag : body;
	return {};
}

[[nodiscard]] QByteArray AssetContext(
		const QString &hash,
		qint64 size,
		qint64 offset) {
	return "asset:" + hash.toLatin1() + ':' + QByteArray::number(size)
		+ ':' + QByteArray::number(offset);
}

[[nodiscard]] Result Save(const QString &path, const QByteArray &bytes) {
	auto file = QSaveFile(path);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)
		|| !file.setPermissions(kOwnerPermissions)
		|| file.write(bytes) != bytes.size() || !file.commit()) {
		return Failure(Error::Io, u"Local data could not be saved."_q);
	}
	return {};
}

[[nodiscard]] Result Read(const QString &path, QByteArray &bytes, qint64 limit) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() < 0
		|| file.size() > limit) {
		return Failure(Error::Io, u"Local data could not be read."_q);
	}
	bytes = file.readAll();
	if (bytes.size() != file.size() || file.error() != QFileDevice::NoError) {
		return Failure(Error::Io, u"Local data read was incomplete."_q);
	}
	return {};
}

[[nodiscard]] Result Parse(const QByteArray &bytes, QJsonObject &object) {
	auto error = QJsonParseError();
	const auto parsed = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !parsed.isObject()) {
		return Failure(Error::Corrupt, u"Local data contains invalid JSON."_q);
	}
	object = parsed.object();
	return {};
}

[[nodiscard]] Result StreamAsset(
		const QString &path,
		const std::shared_ptr<MTP::AuthKey> &key,
		const QString &expected,
		qint64 expectedSize,
		AssetSink sink,
		Cancel cancel) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.read(4) != "TGLA") {
		return Failure(Error::Corrupt, u"Local asset could not be opened."_q);
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	auto count = qint64(0);
	while (count < expectedSize) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto header = file.read(4);
		if (header.size() != 4) {
			return Failure(Error::Corrupt, u"Truncated local asset."_q);
		}
		const auto size = qFromBigEndian<quint32>(header.constData());
		if (size > kAssetChunk + 36 || size < 36) {
			return Failure(Error::Corrupt, u"Invalid local asset block."_q);
		}
		const auto bytes = file.read(size);
		auto plain = QByteArray();
		if (bytes.size() != size) {
			return Failure(Error::Corrupt, u"Truncated local asset block."_q);
		}
		if (auto result = Crypt(false, bytes, plain, key,
			AssetContext(expected, expectedSize, count), kAssetChunk); !result) {
			return result;
		}
		if (plain.isEmpty() || count > expectedSize - plain.size()) {
			return Failure(Error::Corrupt, u"Invalid local asset size."_q);
		}
		count += plain.size();
		hash.addData(plain);
		if (!sink(QByteArrayView(plain))) {
			return Failure(Error::Io, u"Asset consumer stopped."_q);
		}
	}
	if (!file.atEnd() || QString::fromLatin1(hash.result().toHex()) != expected) {
		return Failure(Error::Corrupt, u"Local asset checksum mismatch."_q);
	}
	return {};
}

[[nodiscard]] bool BookkeepingField(const QString &field) {
	return field == u"_telegravim_metadata_variants"_q
		|| field == u"_telegravim_opaque_variants"_q
		|| field == u"_telegravim_conflict_versions"_q
		|| field == u"_telegravim_resolutions"_q;
}

[[nodiscard]] QJsonValue SemanticValue(const QJsonValue &value) {
	if (value.isObject()) {
		auto result = QJsonObject();
		const auto object = value.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (!BookkeepingField(i.key())) {
				result.insert(i.key(), SemanticValue(i.value()));
			}
		}
		return result;
	} else if (value.isArray()) {
		auto result = QJsonArray();
		for (const auto &entry : value.toArray()) {
			result.push_back(SemanticValue(entry));
		}
		return result;
	}
	return value;
}

[[nodiscard]] QJsonObject MergeExtra(
		QJsonObject local,
		const QJsonObject &incoming) {
	for (auto i = incoming.begin(); i != incoming.end(); ++i) {
		if (!local.contains(i.key())) {
			local.insert(i.key(), i.value());
		} else if (BookkeepingField(i.key())
			&& local.value(i.key()).isArray() && i.value().isArray()) {
			auto combined = local.value(i.key()).toArray();
			for (const auto &value : i.value().toArray()) {
				if (!combined.contains(value)) {
					combined.push_back(value);
				}
			}
			local.insert(i.key(), combined);
		} else if (local.value(i.key()) != i.value()) {
			auto retained = local.value(u"_telegravim_metadata_variants"_q).toArray();
			const auto variant = QJsonObject{
				{ u"field"_q, i.key() },
				{ u"value"_q, i.value() },
			};
			if (!retained.contains(variant)) {
				retained.push_back(variant);
			}
			local.insert(u"_telegravim_metadata_variants"_q, retained);
		}
	}
	return local;
}

[[nodiscard]] QJsonObject ResolveMetadata(
		const QJsonObject &local,
		const QJsonObject &incoming,
		ConflictChoice choice) {
	if (choice == ConflictChoice::KeepBoth) {
		return MergeExtra(local, incoming);
	}
	const auto selected = choice == ConflictChoice::UseIncoming
		? incoming : local;
	auto remainder = choice == ConflictChoice::UseIncoming ? local : incoming;
	for (auto i = selected.begin(); i != selected.end(); ++i) {
		if (!BookkeepingField(i.key())) {
			remainder.remove(i.key());
		}
	}
	return MergeExtra(selected, remainder);
}

[[nodiscard]] bool MetadataConflict(
		const QJsonObject &local,
		const QJsonObject &incoming) {
	for (auto i = incoming.begin(); i != incoming.end(); ++i) {
		if (!BookkeepingField(i.key()) && local.contains(i.key())
			&& SemanticValue(local.value(i.key())) != SemanticValue(i.value())) {
			return true;
		}
	}
	return false;
}

bool AddConflict(
		MergePlan &plan,
		const QString &profileId,
		const QString &moduleId,
		const QString &recordId,
		const QJsonObject &local,
		const QJsonObject &incoming) {
	auto localIdentity = SemanticValue(local).toObject();
	auto incomingIdentity = SemanticValue(incoming).toObject();
	if (recordId.startsWith(u'@')) {
		const auto keys = localIdentity.keys();
		for (const auto &key : keys) {
			if (!incomingIdentity.contains(key)
				|| localIdentity.value(key) == incomingIdentity.value(key)) {
				localIdentity.remove(key);
			}
		}
		const auto incomingKeys = incomingIdentity.keys();
		for (const auto &key : incomingKeys) {
			if (!localIdentity.contains(key)) {
				incomingIdentity.remove(key);
			}
		}
	}
	const auto fingerprint = Digest(Json({
		{ u"profile"_q, profileId },
		{ u"module"_q, moduleId },
		{ u"record"_q, recordId },
		{ u"local"_q, localIdentity },
		{ u"incoming"_q, incomingIdentity },
	}));
	if (plan.merged.extra.value(u"_telegravim_resolutions"_q)
		.toArray().contains(fingerprint)) {
		return false;
	}
	plan.conflicts.push_back({
		fingerprint, profileId, moduleId, recordId, local, incoming,
	});
	return true;
}

void MergeMetadata(
		MergePlan &plan,
		const QString &profile,
		const QString &module,
		const QString &scope,
		QJsonObject &local,
		const QJsonObject &incoming) {
	if (MetadataConflict(local, incoming)
		&& !AddConflict(plan, profile, module, scope, local, incoming)) {
		local = ResolveMetadata(local, incoming, ConflictChoice::KeepLocal);
	} else {
		local = MergeExtra(local, incoming);
	}
}

[[nodiscard]] QString ArchiveId(const ArchivedConflict &conflict) {
	return Digest(Json({
		{ u"scope"_q, int(conflict.scope) },
		{ u"profile"_q, conflict.profileId },
		{ u"module"_q, conflict.moduleId },
		{ u"record"_q, conflict.recordId },
		{ u"field"_q, conflict.field },
		{ u"alternate"_q, conflict.alternate },
	}));
}

void WalkArchives(
		const Snapshot &snapshot,
		const std::function<bool(ArchivedConflict)> &visitor) {
	const auto metadata = [&](ArchiveScope scope, const QString &profile,
			const QString &module, const QJsonObject &object) {
		for (const auto &value : object.value(u"_telegravim_metadata_variants"_q).toArray()) {
			const auto variant = value.toObject();
			const auto field = variant.value(u"field"_q).toString();
			if (field.isEmpty() || !variant.contains(u"value"_q)) {
				continue;
			}
			auto conflict = ArchivedConflict{
				.profileId = profile,
				.moduleId = module,
				.current = { { field, object.value(field) } },
				.alternate = { { field, variant.value(u"value"_q) } },
				.scope = scope,
				.field = field,
			};
			conflict.id = ArchiveId(conflict);
			if (!visitor(std::move(conflict))) {
				return false;
			}
		}
		return true;
	};
	if (!metadata(ArchiveScope::RootExtra, {}, {}, snapshot.extra)
		|| !metadata(ArchiveScope::RootTransfer, {}, {}, snapshot.transfer)) {
		return;
	}
	for (const auto &profile : snapshot.profiles) {
		if (!metadata(ArchiveScope::ProfileExtra, profile.id, {}, profile.extra)
			|| !metadata(ArchiveScope::ProfileTransfer, profile.id, {}, profile.transfer)) {
			return;
		}
		for (auto i = profile.modules.begin(); i != profile.modules.end(); ++i) {
			const auto &module = i.value();
			if (!SupportedModule(i.key(), module.value(u"schema_version"_q).toInt())) {
				continue;
			}
			if (!metadata(ArchiveScope::ModuleExtra, profile.id, i.key(), module)) {
				return;
			}
			for (const auto &value : module.value(u"records"_q).toArray()) {
				const auto record = value.toObject();
				for (const auto &alternate : record.value(u"_telegravim_conflict_versions"_q).toArray()) {
					auto conflict = ArchivedConflict{
						.profileId = profile.id,
						.moduleId = i.key(),
						.recordId = record.value(u"id"_q).toString(),
						.current = record,
						.alternate = alternate.toObject(),
					};
					conflict.id = ArchiveId(conflict);
					if (!visitor(std::move(conflict))) {
						return;
					}
				}
			}
		}
	}
	for (const auto &value : snapshot.extra.value(u"_telegravim_opaque_variants"_q).toArray()) {
		const auto variant = value.toObject();
		const auto account = ReadAccount(variant.value(u"account"_q).toObject());
		const auto module = variant.value(u"module"_q).toObject();
		const auto moduleId = module.value(u"module_id"_q).toString();
		for (const auto &profile : snapshot.profiles) {
			if (profile.account != account) {
				continue;
			}
			auto conflict = ArchivedConflict{
				.profileId = profile.id,
				.moduleId = moduleId,
				.current = profile.modules.value(moduleId),
				.alternate = module,
				.scope = ArchiveScope::Module,
			};
			conflict.id = ArchiveId(conflict);
			if (!visitor(std::move(conflict))) {
				return;
			}
			break;
		}
	}
	for (auto i = snapshot.assets.begin(); i != snapshot.assets.end(); ++i) {
		if (!metadata(ArchiveScope::AssetExtra, {}, i.key(), i->extra)
			|| !metadata(ArchiveScope::AssetTransfer, {}, i.key(), i->transfer)) {
			return;
		}
	}
}

[[nodiscard]] Result NormalizeBindings(
		QJsonObject &module,
		const QString &device) {
	if (module.value(u"module_id"_q).toString() != u"saved_tags"_q
		|| (module.value(u"schema_version"_q).toInt() != 1
			&& module.value(u"schema_version"_q).toInt() != 2)) {
		return {};
	}
	auto records = Records(module);
	auto active = QMap<QString, QString>();
	for (auto i = records.begin(); i != records.end(); ++i) {
		auto &record = i.value();
		if (record.value(u"kind"_q).toString() != u"binding"_q
			|| record.value(u"deleted"_q).toBool()) {
			continue;
		}
		const auto tagId = record.value(u"tag_id"_q).toString();
		const auto tag = records.value(tagId);
		const auto message = record.value(u"message"_q).toObject();
		const auto identity = tagId + u":"_q
			+ message.value(u"message_id"_q).toString();
		const auto duplicate = active.value(identity);
		if (tag.value(u"deleted"_q).toBool() || !duplicate.isEmpty()) {
			auto previous = record;
			previous.remove(u"_telegravim_conflict_versions"_q);
			auto variants = record.value(u"_telegravim_conflict_versions"_q).toArray();
			if (!variants.contains(previous)) {
				variants.push_back(previous);
			}
			record.insert(u"_telegravim_conflict_versions"_q, variants);
			record.insert(u"deleted"_q, true);
			if (!duplicate.isEmpty()) {
				record.insert(u"_telegravim_duplicate_of"_q, duplicate);
			}
			if (auto result = Advance(record, device); !result) {
				return result;
			}
		} else {
			active.insert(identity, i.key());
		}
	}
	SetRecords(module, records);
	return {};
}

} // namespace

QString NewId() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool ValidAccount(const Account &account) {
	if ((account.environment != u"production"_q
		&& account.environment != u"test"_q) || !Decimal(account.userId)) {
		return false;
	}
	auto ok = false;
	const auto number = account.userId.toULongLong(&ok);
	return ok && number != 0;
}

bool SupportedModule(const QString &id, int version) {
	return Definition(id, version).has_value();
}

bool ValidModuleId(const QString &id) {
	return ModuleId(id);
}

Result RegisterModule(ModuleDefinition definition) {
	if (!ModuleId(definition.id) || definition.schemaVersion < 1
		|| !definition.validate || !definition.assetReferences) {
		return Failure(Error::Invalid, u"Invalid local module registration."_q);
	}
	const auto lock = std::lock_guard(RegistryMutex());
	if (Registry().contains(definition.id)) {
		return Failure(Error::Conflict, u"Local module is already registered."_q);
	}
	const auto id = definition.id;
	Registry().insert(id, std::move(definition));
	return {};
}

QJsonObject EmptyModule(const QString &id) {
	const auto definition = Definition(id, -1);
	return {
		{ u"module_id"_q, id },
		{ u"schema_version"_q, definition ? definition->schemaVersion : 1 },
		{ u"records"_q, QJsonArray() },
	};
}

QJsonObject ReadModule(
		const Snapshot &snapshot,
		const Account &account,
		const QString &moduleId) {
	for (const auto &profile : snapshot.profiles) {
		if (profile.account == account) {
			return profile.modules.value(moduleId, EmptyModule(moduleId));
		}
	}
	return EmptyModule(moduleId);
}

std::vector<ArchivedConflict> ArchivedConflicts(
		const Snapshot &snapshot,
		int offset,
		int limit) {
	auto result = std::vector<ArchivedConflict>();
	if (offset < 0 || limit <= 0) {
		return result;
	}
	limit = std::min(limit, 100);
	WalkArchives(snapshot, [&](ArchivedConflict conflict) {
		if (offset > 0) {
			--offset;
		} else {
			result.push_back(std::move(conflict));
		}
		return result.size() < limit;
	});
	return result;
}

int ArchivedConflictCount(const Snapshot &snapshot) {
	auto result = 0;
	WalkArchives(snapshot, [&](ArchivedConflict) {
		++result;
		return true;
	});
	return result;
}

Result ValidateSnapshot(
		const Snapshot &snapshot,
		const Limits &limits,
		Cancel cancel) {
	if (snapshot.profiles.size() > limits.maxProfiles
		|| snapshot.assets.size() > limits.maxAssets
		|| !BoundedJson(snapshot.extra, limits.maxDepth)
		|| !BoundedJson(snapshot.transfer, limits.maxDepth)) {
		return Failure(Error::Invalid, u"Local data exceeds its limits."_q);
	}
	auto total = qint64(Json(snapshot.extra).size() + Json(snapshot.transfer).size());
	auto recordCount = qint64(0);
	auto accounts = QSet<QString>();
	for (auto i = snapshot.profiles.begin(); i != snapshot.profiles.end(); ++i) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto &profile = i.value();
		const auto account = AccountId(profile.account);
		if (!ValidId(profile.id) || i.key() != profile.id
			|| !ValidAccount(profile.account) || accounts.contains(account)
			|| profile.modules.size() > limits.maxModules
			|| !BoundedJson(profile.extra, limits.maxDepth)
			|| !BoundedJson(profile.transfer, limits.maxDepth)) {
			return Failure(Error::Invalid, u"Invalid account profile."_q);
		}
		accounts.insert(account);
		total += Json(profile.extra).size() + Json(profile.transfer).size();
		for (auto m = profile.modules.begin(); m != profile.modules.end(); ++m) {
			const auto &module = m.value();
			const auto id = module.value(u"module_id"_q).toString();
			const auto version = module.value(u"schema_version"_q).toDouble();
			if (!ModuleId(id) || m.key() != id || version < 1.
				|| version > 2147483647. || std::floor(version) != version
				|| !BoundedJson(module, limits.maxDepth)) {
				return Failure(Error::Invalid, u"Invalid module envelope."_q);
			}
			const auto size = Json(module).size();
			total += size;
			if (size > limits.maxModuleBytes || total > limits.maxJsonBytes) {
				return Failure(Error::Invalid, u"Local JSON exceeds its size limit."_q);
			}
			if (const auto definition = Definition(id, int(version))) {
				recordCount += module.value(u"records"_q).toArray().size();
				if (recordCount > limits.maxRecords) {
					return Failure(Error::Invalid, u"Too many local records."_q);
				}
				if (auto result = ValidateTags(module, profile.account, limits, cancel)
				; !result) {
					return result;
				}
				if (auto result = definition->validate(module, profile.account); !result) {
					return result;
				}
				for (const auto &asset : definition->assetReferences(module)) {
					if (!ValidHash(asset) || !snapshot.assets.contains(asset)) {
						return Failure(Error::Invalid, u"Module attachment is missing."_q);
					}
				}
			}
		}
	}
	auto assetBytes = qint64(0);
	for (auto i = snapshot.assets.begin(); i != snapshot.assets.end(); ++i) {
		const auto &asset = i.value();
		if (!ValidHash(asset.sha256) || i.key() != asset.sha256 || asset.size < 0
			|| asset.size > limits.maxAssetBytes
			|| !BoundedJson(asset.extra, limits.maxDepth)
			|| !BoundedJson(asset.transfer, limits.maxDepth)
			|| assetBytes > limits.maxAssetTotalBytes - asset.size) {
			return Failure(Error::Invalid, u"Invalid asset or asset size limit."_q);
		}
		assetBytes += asset.size;
		total += Json(asset.extra).size() + Json(asset.transfer).size();
	}
	if (total > limits.maxJsonBytes) {
		return Failure(Error::Invalid, u"Local JSON exceeds its size limit."_q);
	}
	return LocalTransferFormat::ValidateShape(snapshot, cancel);
}

class Store::Private {
public:
	Private(QString path, std::shared_ptr<MTP::AuthKey> key)
	: path(std::move(path)), key(std::move(key)) {
	}

	[[nodiscard]] Result load(Cancel cancel);
	[[nodiscard]] Result commit(Snapshot next, Cancel cancel);
	[[nodiscard]] Result writeBlob(const QJsonObject &object, QString &hash);
	[[nodiscard]] Result readBlob(const QString &hash, QJsonObject &object);
	[[nodiscard]] Result writeAsset(const Asset &asset, Cancel cancel);
	[[nodiscard]] Result verifyFiles(Cancel cancel);
	[[nodiscard]] Asset attachReader(Asset asset) const;
	[[nodiscard]] Result initialize();
	[[nodiscard]] Result markInitialized();

	QString path;
	std::shared_ptr<MTP::AuthKey> key;
	std::mutex mutex;
	Snapshot state;
	QByteArray currentBytes;
	QMap<QString, FileStamp> verifiedBlobs;
	QMap<QString, std::pair<FileStamp, qint64>> verifiedAssets;
	QSet<QString> activeBlobs;
	Limits limits;

};

Result Store::Private::initialize() {
	if (!key) {
		return Failure(Error::Locked, u"Unlock the account before accessing local data."_q);
	}
	for (const auto &folder : { path, path + u"/blobs"_q, path + u"/assets"_q }) {
		if (!QDir().mkpath(folder)
			|| !QFile::setPermissions(folder, kOwnerPermissions
				| QFileDevice::ExeOwner)) {
			return Failure(Error::Io, u"Local data directory is unavailable."_q);
		}
	}
	return {};
}

Result Store::Private::markInitialized() {
	return QFile::exists(path + u"/initialized"_q)
		? Result()
		: Save(path + u"/initialized"_q, "1");
}

Result Store::Private::writeBlob(const QJsonObject &object, QString &hash) {
	const auto bytes = Json(object);
	hash = Digest(bytes);
	const auto location = path + u"/blobs/"_q + hash;
	const auto stamp = FileIdentity(location);
	const auto cached = verifiedBlobs.constFind(hash);
	if (!stamp && (cached != verifiedBlobs.cend() || activeBlobs.contains(hash))) {
		return Failure(Error::Corrupt, u"Local data segment is missing or inaccessible."_q);
	} else if (stamp && cached != verifiedBlobs.cend() && *stamp == *cached) {
		return {};
	}
	if (QFile::exists(location)) {
		auto existing = QJsonObject();
		if (auto result = readBlob(hash, existing); !result) {
			return result;
		}
		if (existing != object) {
			return Failure(Error::Corrupt, u"Local data checksum mismatch."_q);
		}
		return {};
	} else {
		auto encrypted = QByteArray();
		if (auto result = Crypt(true, bytes, encrypted, key,
			"segment:" + hash.toLatin1(), limits.maxModuleBytes); !result) {
			return result;
		}
		if (auto result = Save(location, encrypted); !result) {
			return result;
		}
	}
	const auto written = FileIdentity(location);
	if (!written) {
		return Failure(Error::Corrupt, u"Local data segment disappeared."_q);
	}
	verifiedBlobs.insert(hash, *written);
	return {};
}

Result Store::Private::readBlob(const QString &hash, QJsonObject &object) {
	if (!ValidHash(hash)) {
		return Failure(Error::Corrupt, u"Invalid local segment identity."_q);
	}
	const auto location = path + u"/blobs/"_q + hash;
	const auto stamp = FileIdentity(location);
	if (!stamp) {
		return Failure(Error::Corrupt, u"Local data segment is missing or inaccessible."_q);
	}
	auto encrypted = QByteArray();
	if (auto result = Read(location,
		encrypted, limits.maxModuleBytes + 64); !result) {
		return result;
	}
	auto bytes = QByteArray();
	if (auto result = Crypt(false, encrypted, bytes, key,
		"segment:" + hash.toLatin1(), limits.maxModuleBytes); !result) {
		return result;
	}
	if (Digest(bytes) != hash) {
		return Failure(Error::Corrupt, u"Local segment checksum mismatch."_q);
	}
	if (auto result = Parse(bytes, object); !result) {
		return result;
	}
	if (FileIdentity(location) != stamp) {
		return Failure(Error::Corrupt, u"Local data segment changed while reading."_q);
	}
	verifiedBlobs.insert(hash, *stamp);
	return {};
}

Asset Store::Private::attachReader(Asset asset) const {
	const auto location = path + u"/assets/"_q + asset.sha256;
	const auto localKey = key;
	const auto hash = asset.sha256;
	const auto size = asset.size;
	asset.read = [=](AssetSink sink, Cancel cancel) {
		return StreamAsset(location, localKey, hash, size, std::move(sink), cancel);
	};
	return asset;
}

Result Store::Private::writeAsset(const Asset &asset, Cancel cancel) {
	const auto location = path + u"/assets/"_q + asset.sha256;
	const auto stamp = FileIdentity(location);
	const auto cached = verifiedAssets.constFind(asset.sha256);
	if (!stamp && (cached != verifiedAssets.cend() || state.assets.contains(asset.sha256))) {
		return Failure(Error::Corrupt, u"Local asset is missing or inaccessible."_q);
	} else if (stamp && cached != verifiedAssets.cend()
		&& *stamp == cached->first && asset.size == cached->second) {
		return {};
	}
	if (QFile::exists(location)) {
		const auto result = StreamAsset(location, key, asset.sha256, asset.size,
			[](QByteArrayView) { return true; }, cancel);
		if (!result) {
			return result;
		} else if (!stamp || FileIdentity(location) != stamp) {
			return Failure(Error::Corrupt, u"Local asset changed while reading."_q);
		}
		verifiedAssets.insert(asset.sha256, { *stamp, asset.size });
		return {};
	} else if (!asset.read) {
		return Failure(Error::Invalid, u"Asset data is missing."_q);
	}
	auto file = QSaveFile(location);
	file.setDirectWriteFallback(false);
	if (!file.open(QIODevice::WriteOnly)
		|| !file.setPermissions(kOwnerPermissions) || file.write("TGLA", 4) != 4) {
		return Failure(Error::Io, u"Local asset could not be created."_q);
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	auto count = qint64(0);
	const auto result = asset.read([&](QByteArrayView bytes) {
		if (Cancelled(cancel) || count > asset.size - bytes.size()) {
			return false;
		}
		hash.addData(bytes);
		while (!bytes.empty()) {
			const auto part = bytes.first(std::min<qsizetype>(bytes.size(), kAssetChunk));
			auto encrypted = QByteArray();
			if (!Crypt(true, QByteArray(part.data(), part.size()), encrypted, key,
				AssetContext(asset.sha256, asset.size, count), kAssetChunk)) {
				return false;
			}
			const auto length = qToBigEndian(quint32(encrypted.size()));
			if (file.write(reinterpret_cast<const char*>(&length), 4) != 4
				|| file.write(encrypted) != encrypted.size()) {
				return false;
			}
			bytes = bytes.sliced(part.size());
			count += part.size();
		}
		return true;
	}, cancel);
	if (Cancelled(cancel)) {
		return Cancellation();
	} else if (!result) {
		return result;
	} else if (count != asset.size
		|| QString::fromLatin1(hash.result().toHex()) != asset.sha256) {
		return Failure(Error::Corrupt, u"Imported asset checksum mismatch."_q);
	} else if (!file.commit()) {
		return Failure(Error::Io, u"Local asset could not be saved."_q);
	}
	const auto written = FileIdentity(location);
	if (!written) {
		return Failure(Error::Corrupt, u"Local asset disappeared."_q);
	}
	verifiedAssets.insert(asset.sha256, { *written, asset.size });
	return {};
}

Result Store::Private::verifyFiles(Cancel cancel) {
	for (const auto &hash : activeBlobs) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto stamp = FileIdentity(path + u"/blobs/"_q + hash);
		if (!stamp) {
			return Failure(Error::Corrupt, u"Local data segment is missing or inaccessible."_q);
		}
		const auto cached = verifiedBlobs.constFind(hash);
		if (cached == verifiedBlobs.cend() || *cached != *stamp) {
			auto ignored = QJsonObject();
			if (auto result = readBlob(hash, ignored); !result) {
				return result;
			}
		}
	}
	for (const auto &asset : state.assets) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		if (auto result = writeAsset(asset, cancel); !result) {
			return result;
		}
	}
	return {};
}

Result Store::Private::load(Cancel cancel) {
	if (auto result = initialize(); !result) {
		return result;
	}
	const auto current = path + u"/current.bin"_q;
	if (!QFile::exists(current)) {
		if (!currentBytes.isEmpty() || QFile::exists(path + u"/previous.bin"_q)
			|| QFile::exists(path + u"/initialized"_q) || HasStoredFiles(path)) {
			return Failure(Error::Corrupt, u"Local data manifest is missing; recovery is required."_q);
		}
		if (state.deviceId.isEmpty()) {
			state.deviceId = NewId();
			state.generation = NewId();
		}
		if (auto result = commit(state, cancel); !result) {
			return result;
		}
		return markInitialized();
	}
	auto encrypted = QByteArray();
	if (auto result = Read(current, encrypted, limits.maxJsonBytes + 64); !result) {
		return result;
	}
	if (encrypted == currentBytes) {
		if (auto result = verifyFiles(cancel); !result) {
			return result;
		}
		return markInitialized();
	}
	auto plain = QByteArray();
	if (auto result = Crypt(false, encrypted, plain, key,
		"manifest", limits.maxJsonBytes); !result) {
		return result;
	}
	auto root = QJsonObject();
	if (auto result = Parse(plain, root); !result) {
		return result;
	}
	if (root.value(u"format"_q).toString() != u"telegravim-local"_q
		|| root.value(u"version"_q).toInt() != 1
		|| !BoundedJson(root, limits.maxDepth + 3)) {
		return Failure(Error::Unsupported, u"Unsupported local data format."_q);
	}
	if (!root.value(u"extra"_q).isObject()
		|| !root.value(u"transfer"_q).isObject()
		|| !root.value(u"profiles"_q).isArray()
		|| !root.value(u"assets"_q).isArray()) {
		return Failure(Error::Corrupt, u"Invalid local data manifest."_q);
	}
	auto loaded = Snapshot();
	auto loadedBlobs = QSet<QString>();
	loaded.generation = root.value(u"generation"_q).toString();
	loaded.deviceId = root.value(u"device_id"_q).toString();
	loaded.extra = root.value(u"extra"_q).toObject();
	loaded.transfer = root.value(u"transfer"_q).toObject();
	if (!ValidId(loaded.generation) || !ValidId(loaded.deviceId)) {
		return Failure(Error::Corrupt, u"Invalid local data generation."_q);
	}
	auto total = qint64(Json(loaded.extra).size() + Json(loaded.transfer).size());
	if (root.value(u"profiles"_q).toArray().size() > limits.maxProfiles
		|| root.value(u"assets"_q).toArray().size() > limits.maxAssets) {
		return Failure(Error::Corrupt, u"Local data manifest exceeds its limits."_q);
	}
	for (const auto &p : root.value(u"profiles"_q).toArray()) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto object = p.toObject();
		auto profile = Profile();
		profile.id = object.value(u"id"_q).toString();
		profile.account = ReadAccount(object.value(u"account"_q).toObject());
		profile.extra = object.value(u"extra"_q).toObject();
		profile.transfer = object.value(u"transfer"_q).toObject();
		total += Json(profile.extra).size() + Json(profile.transfer).size();
		if (!object.value(u"account"_q).isObject()
			|| !object.value(u"extra"_q).isObject()
			|| !object.value(u"transfer"_q).isObject()
			|| !object.value(u"modules"_q).isArray()
			|| object.value(u"modules"_q).toArray().size() > limits.maxModules
			|| total > limits.maxJsonBytes) {
			return Failure(Error::Corrupt, u"Too many local modules."_q);
		}
		for (const auto &m : object.value(u"modules"_q).toArray()) {
			const auto entry = m.toObject();
			const auto id = entry.value(u"id"_q).toString();
			if (!entry.value(u"opaque"_q).isBool()
				|| !entry.value(u"segments"_q).isArray()
				|| entry.value(u"segments"_q).toArray().size() > 256) {
				return Failure(Error::Corrupt, u"Invalid local module segments."_q);
			}
			auto module = QJsonObject();
			if (auto result = readBlob(entry.value(u"meta"_q).toString(), module)
			; !result) {
				return result;
			}
			loadedBlobs.insert(entry.value(u"meta"_q).toString());
			if (!entry.value(u"opaque"_q).toBool()) {
				auto records = QJsonArray();
				module.insert(u"records"_q, records);
				auto moduleBytes = qint64(Json(module).size());
				if (total + moduleBytes > limits.maxJsonBytes) {
					return Failure(Error::Corrupt, u"Local JSON exceeds its size limit."_q);
				}
				for (const auto &segment : entry.value(u"segments"_q).toArray()) {
					if (Cancelled(cancel)) {
						return Cancellation();
					}
					auto shard = QJsonObject();
					if (auto result = readBlob(segment.toString(), shard); !result) {
						return result;
					}
					loadedBlobs.insert(segment.toString());
					if (!shard.value(u"records"_q).isArray()) {
						return Failure(Error::Corrupt, u"Invalid local record segment."_q);
					}
					for (const auto &record : shard.value(u"records"_q).toArray()) {
						if (!record.isObject()) {
							return Failure(Error::Corrupt, u"Invalid local record."_q);
						}
						moduleBytes += Json(record.toObject()).size()
							+ (records.isEmpty() ? 0 : 1);
						if (moduleBytes > limits.maxModuleBytes
							|| total + moduleBytes > limits.maxJsonBytes) {
							return Failure(Error::Corrupt, u"Local JSON exceeds its size limit."_q);
						}
						records.push_back(record);
					}
				}
				module.insert(u"records"_q, records);
			}
			total += Json(module).size();
			if (total > limits.maxJsonBytes) {
				return Failure(Error::Corrupt, u"Local JSON exceeds its size limit."_q);
			}
			if (profile.modules.contains(id)) {
				return Failure(Error::Corrupt, u"Duplicate local module."_q);
			}
			profile.modules.insert(id, module);
		}
		if (loaded.profiles.contains(profile.id)) {
			return Failure(Error::Corrupt, u"Duplicate local profile."_q);
		}
		loaded.profiles.insert(profile.id, profile);
	}
	for (const auto &value : root.value(u"assets"_q).toArray()) {
		const auto object = value.toObject();
		auto asset = Asset();
		asset.sha256 = object.value(u"sha256"_q).toString();
		auto ok = false;
		asset.size = object.value(u"size"_q).toString().toLongLong(&ok);
		asset.extra = object.value(u"extra"_q).toObject();
		asset.transfer = object.value(u"transfer"_q).toObject();
		total += Json(asset.extra).size() + Json(asset.transfer).size();
		if (!ok || !ValidHash(asset.sha256) || asset.size < 0
			|| asset.size > limits.maxAssetBytes
			|| !object.value(u"extra"_q).isObject()
			|| !object.value(u"transfer"_q).isObject()
			|| loaded.assets.contains(asset.sha256)
			|| total > limits.maxJsonBytes) {
			return Failure(Error::Corrupt, u"Invalid local asset entry."_q);
		}
		if (!FileIdentity(path + u"/assets/"_q + asset.sha256)) {
			return Failure(Error::Corrupt, u"Local asset is missing or inaccessible."_q);
		}
		if (auto result = writeAsset(asset, cancel); !result) {
			return result;
		}
		const auto hash = asset.sha256;
		loaded.assets.insert(hash, attachReader(std::move(asset)));
	}
	if (auto result = ValidateSnapshot(loaded, limits, cancel); !result) {
		return result;
	}
	if (auto result = markInitialized(); !result) {
		return result;
	}
	state = std::move(loaded);
	activeBlobs = std::move(loadedBlobs);
	currentBytes = std::move(encrypted);
	return {};
}

Result Store::Private::commit(Snapshot next, Cancel cancel) {
	if (auto result = ValidateSnapshot(next, limits, cancel); !result) {
		return result;
	}
	auto lock = QLockFile(path + u"/write.lock"_q);
	if (!lock.tryLock(0)) {
		return Failure(Error::Locked, u"Local data is being changed by another process."_q);
	}
	auto disk = QByteArray();
	const auto current = path + u"/current.bin"_q;
	if (QFile::exists(current)) {
		if (auto result = Read(current, disk, limits.maxJsonBytes + 64); !result) {
			return result;
		}
	}
	if (disk != currentBytes) {
		return Failure(Error::Stale, u"Local data changed; reload and try again."_q);
	}
	next.deviceId = state.deviceId;
	next.generation = NewId();
	auto nextBlobs = QSet<QString>();
	auto profiles = QJsonArray();
	for (const auto &profile : next.profiles) {
		auto modules = QJsonArray();
		for (auto m = profile.modules.begin(); m != profile.modules.end(); ++m) {
			if (Cancelled(cancel)) {
				return Cancellation();
			}
			auto meta = m.value();
			const auto supported = SupportedModule(m.key(), meta.value(u"schema_version"_q).toInt());
			auto segments = QJsonArray();
			if (supported) {
				auto buckets = QMap<QString, QJsonArray>();
				for (const auto &record : meta.value(u"records"_q).toArray()) {
					const auto id = record.toObject().value(u"id"_q).toString();
					buckets[id.left(2)].push_back(record);
				}
				meta.remove(u"records"_q);
				for (const auto &records : buckets) {
					auto hash = QString();
					if (auto result = writeBlob({ { u"records"_q, records } }, hash); !result) {
						return result;
					}
					segments.push_back(hash);
					nextBlobs.insert(hash);
				}
			}
			auto hash = QString();
			if (auto result = writeBlob(meta, hash); !result) {
				return result;
			}
			nextBlobs.insert(hash);
			modules.push_back(QJsonObject{
				{ u"id"_q, m.key() },
				{ u"meta"_q, hash },
				{ u"opaque"_q, !supported },
				{ u"segments"_q, segments },
			});
		}
		profiles.push_back(QJsonObject{
			{ u"id"_q, profile.id },
			{ u"account"_q, AccountJson(profile.account) },
			{ u"extra"_q, profile.extra },
			{ u"transfer"_q, profile.transfer },
			{ u"modules"_q, modules },
		});
	}
	auto assets = QJsonArray();
	for (auto i = next.assets.begin(); i != next.assets.end(); ++i) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		auto &asset = i.value();
		if (auto result = writeAsset(asset, cancel); !result) {
			return result;
		}
		asset = attachReader(std::move(asset));
		assets.push_back(QJsonObject{
			{ u"sha256"_q, asset.sha256 },
			{ u"size"_q, QString::number(asset.size) },
			{ u"extra"_q, asset.extra },
			{ u"transfer"_q, asset.transfer },
		});
	}
	const auto root = QJsonObject{
		{ u"format"_q, u"telegravim-local"_q },
		{ u"version"_q, 1 },
		{ u"generation"_q, next.generation },
		{ u"device_id"_q, next.deviceId },
		{ u"extra"_q, next.extra },
		{ u"transfer"_q, next.transfer },
		{ u"profiles"_q, profiles },
		{ u"assets"_q, assets },
	};
	auto bytes = QByteArray();
	if (auto result = Crypt(true, Json(root), bytes, key,
		"manifest", limits.maxJsonBytes); !result) {
		return result;
	}
	if (Cancelled(cancel)) {
		return Cancellation();
	}
	if (!currentBytes.isEmpty()) {
		if (auto result = Save(path + u"/previous.bin"_q, currentBytes); !result) {
			return result;
		}
	}
	if (auto result = Save(current, bytes); !result) {
		return result;
	}
	state = std::move(next);
	activeBlobs = std::move(nextBlobs);
	currentBytes = bytes;
	return {};
}

Store::Store(QString path, std::shared_ptr<MTP::AuthKey> key)
: _private(std::make_unique<Private>(std::move(path), std::move(key))) {
}

Store::~Store() = default;

Result Store::snapshot(Snapshot &result, Cancel cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if (auto loaded = _private->load(cancel); !loaded) {
		return loaded;
	}
	result = _private->state;
	return {};
}

Result Store::mutate(
		const Account &account,
		const QString &moduleId,
		std::function<Result(QJsonObject &)> change,
		Cancel cancel) {
	return mutateWithAssets(account, moduleId, std::move(change), {}, cancel);
}

Result Store::mutateWithAssets(
		const Account &account,
		const QString &moduleId,
		std::function<Result(QJsonObject &)> change,
		const QMap<QString, Asset> &assets,
		Cancel cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if (!ValidAccount(account) || !Definition(moduleId, -1)) {
		return Failure(Error::Invalid, u"Unsupported account or module."_q);
	}
	if (auto result = _private->load(cancel); !result) {
		return result;
	}
	auto next = _private->state;
	for (auto i = assets.begin(); i != assets.end(); ++i) {
		if (next.assets.contains(i.key()) && next.assets.value(i.key()).size != i->size) {
			return Failure(Error::Invalid, u"Asset size conflicts with existing data."_q);
		}
		next.assets.insert(i.key(), i.value());
	}
	auto profileId = QString();
	for (const auto &profile : next.profiles) {
		if (profile.account == account) {
			profileId = profile.id;
			break;
		}
	}
	if (profileId.isEmpty()) {
		profileId = NewId();
		next.profiles.insert(profileId, Profile{ profileId, account });
	}
	auto &profile = next.profiles[profileId];
	const auto before = profile.modules.value(moduleId, EmptyModule(moduleId));
	if (!SupportedModule(moduleId, before.value(u"schema_version"_q).toInt())) {
		return Failure(Error::Unsupported, u"This module requires a newer application."_q);
	}
	auto module = before;
	if (auto result = change(module); !result) {
		return result;
	}
	if (module == before && assets.isEmpty()) {
		return {};
	}
	auto after = QMap<QString, QJsonObject>();
	const auto previous = Records(before);
	for (const auto &value : module.value(u"records"_q).toArray()) {
		auto record = value.toObject();
		auto id = record.value(u"id"_q).toString();
		if (id.isEmpty()) {
			id = NewId();
			record.insert(u"id"_q, id);
		}
		if (!ValidId(id) || after.contains(id)) {
			return Failure(Error::Invalid, u"Duplicate or invalid record identity."_q);
		}
		if (!record.contains(u"deleted"_q)) {
			record.insert(u"deleted"_q, false);
		}
		if (!previous.contains(id) || previous.value(id) != record) {
			record.insert(u"version_vector"_q,
				previous.value(id).value(u"version_vector"_q).toObject());
			if (auto result = Advance(record, next.deviceId); !result) {
				return result;
			}
		}
		after.insert(id, record);
	}
	for (auto i = previous.begin(); i != previous.end(); ++i) {
		if (!after.contains(i.key())) {
			auto deleted = i.value();
			if (!deleted.value(u"deleted"_q).toBool()) {
				deleted.insert(u"deleted"_q, true);
				if (auto result = Advance(deleted, next.deviceId); !result) {
					return result;
				}
			}
			after.insert(i.key(), deleted);
		}
	}
	SetRecords(module, after);
	if (auto result = NormalizeBindings(module, next.deviceId); !result) {
		return result;
	}
	profile.modules.insert(moduleId, module);
	if (auto result = _private->commit(std::move(next), cancel); !result) {
		return result;
	}
	crl::on_main([weak = weak_from_this()] {
		if (const auto strong = weak.lock()) {
			strong->_changes.fire({});
		}
	});
	return {};
}

Result Store::prepareImport(
		const Snapshot &incoming,
		MergePlan &plan,
		Cancel cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if (auto result = ValidateSnapshot(incoming, _private->limits, cancel); !result) {
		return result;
	}
	if (auto result = _private->load(cancel); !result) {
		return result;
	}
	plan = MergePlan();
	plan.baseGeneration = _private->state.generation;
	plan.merged = _private->state;
	MergeMetadata(plan, {}, {}, u"@root-extra"_q,
		plan.merged.extra, incoming.extra);
	MergeMetadata(plan, {}, {}, u"@root-transfer"_q,
		plan.merged.transfer, incoming.transfer);
	for (const auto &source : incoming.profiles) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		plan.accounts.push_back(AccountId(source.account));
		auto profileId = QString();
		for (const auto &profile : plan.merged.profiles) {
			if (profile.account == source.account) {
				profileId = profile.id;
				break;
			}
		}
		if (profileId.isEmpty()) {
			profileId = plan.merged.profiles.contains(source.id) ? NewId() : source.id;
			plan.merged.profiles.insert(profileId, Profile{ profileId, source.account });
		}
		auto &target = plan.merged.profiles[profileId];
		MergeMetadata(plan, profileId, {}, u"@profile-extra"_q,
			target.extra, source.extra);
		MergeMetadata(plan, profileId, {}, u"@profile-transfer"_q,
			target.transfer, source.transfer);
		for (auto m = source.modules.begin(); m != source.modules.end(); ++m) {
			const auto &id = m.key();
			const auto &from = m.value();
			const auto supported = SupportedModule(id,
				from.value(u"schema_version"_q).toInt());
			if (!supported) {
				plan.unsupportedModules.push_back(id);
			}
			if (!target.modules.contains(id)) {
				target.modules.insert(id, from);
				continue;
			}
			auto &to = target.modules[id];
			if (to == from) {
				continue;
			} else if (!supported
				|| !SupportedModule(id, to.value(u"schema_version"_q).toInt())) {
				AddConflict(plan, profileId, id, {}, to, from);
				continue;
			}
			auto localMeta = to;
			auto remoteMeta = from;
			if (id == u"saved_tags"_q) {
				localMeta.insert(u"schema_version"_q, 2);
				remoteMeta.insert(u"schema_version"_q, 2);
			}
			localMeta.remove(u"records"_q);
			remoteMeta.remove(u"records"_q);
			MergeMetadata(plan, profileId, id, u"@module-extra"_q,
				localMeta, remoteMeta);
			auto records = Records(to);
			for (const auto &value : from.value(u"records"_q).toArray()) {
				if (Cancelled(cancel)) {
					return Cancellation();
				}
				const auto remote = value.toObject();
				const auto recordId = remote.value(u"id"_q).toString();
				const auto local = records.constFind(recordId);
				if (local == records.cend()) {
					records.insert(recordId, remote);
					continue;
				}
				const auto ordering = Compare(*local, remote);
				if (ordering == Ordering::Before) {
					records[recordId] = remote;
				} else if (ordering == Ordering::Concurrent
					|| (ordering == Ordering::Equal && *local != remote)) {
					AddConflict(plan, profileId, id, recordId, *local, remote);
				}
			}
			to = std::move(localMeta);
			SetRecords(to, records);
		}
	}
	for (auto i = incoming.assets.begin(); i != incoming.assets.end(); ++i) {
		const auto existing = plan.merged.assets.find(i.key());
		if (existing == plan.merged.assets.end()) {
			plan.merged.assets.insert(i.key(), i.value());
		} else if (existing->size != i->size) {
			return Failure(Error::Invalid, u"Imported asset size conflicts with existing data."_q);
		} else {
			MergeMetadata(plan, {}, i.key(), u"@asset-extra"_q,
				existing->extra, i->extra);
			MergeMetadata(plan, {}, i.key(), u"@asset-transfer"_q,
				existing->transfer, i->transfer);
		}
	}
	plan.unsupportedModules.removeDuplicates();
	return ValidateSnapshot(plan.merged, _private->limits, cancel);
}

Result Store::applyImport(
		const MergePlan &plan,
		const QMap<QString, ConflictChoice> &choices,
		Cancel cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if (auto result = _private->load(cancel); !result) {
		return result;
	}
	if (plan.baseGeneration != _private->state.generation) {
		return Failure(Error::Stale, u"Local data changed; review the import again."_q);
	}
	auto next = plan.merged;
	for (const auto &conflict : plan.conflicts) {
		if (Cancelled(cancel)) {
			return Cancellation();
		}
		const auto choice = choices.constFind(conflict.id);
		if (choice == choices.cend()) {
			return Failure(Error::Conflict, u"Choose a resolution for every conflict."_q);
		}
		const auto selected = *choice == ConflictChoice::UseIncoming
			? conflict.incoming : conflict.local;
		const auto metadata = ResolveMetadata(
			conflict.local,
			conflict.incoming,
			*choice);
		if (conflict.recordId == u"@root-extra"_q) {
			next.extra = metadata;
		} else if (conflict.recordId == u"@root-transfer"_q) {
			next.transfer = metadata;
		} else if (conflict.recordId == u"@asset-extra"_q
			|| conflict.recordId == u"@asset-transfer"_q) {
			const auto asset = next.assets.find(conflict.moduleId);
			if (asset == next.assets.end()) {
				return Failure(Error::Invalid, u"Import asset disappeared."_q);
			}
			if (conflict.recordId == u"@asset-extra"_q) {
				asset->extra = metadata;
			} else {
				asset->transfer = metadata;
			}
		} else {
			auto p = next.profiles.find(conflict.profileId);
			if (p == next.profiles.end()) {
				return Failure(Error::Invalid, u"Import profile disappeared."_q);
			}
			if (conflict.recordId == u"@profile-extra"_q) {
				p->extra = metadata;
			} else if (conflict.recordId == u"@profile-transfer"_q) {
				p->transfer = metadata;
			} else if (conflict.recordId.isEmpty()) {
				p->modules[conflict.moduleId] = selected;
				if (*choice == ConflictChoice::KeepBoth) {
					auto retained = next.extra.value(u"_telegravim_opaque_variants"_q).toArray();
					const auto variant = QJsonObject{
						{ u"id"_q, conflict.id },
						{ u"account"_q, AccountJson(p->account) },
						{ u"module"_q, conflict.incoming },
					};
					if (!retained.contains(variant)) {
						retained.push_back(variant);
					}
					next.extra.insert(u"_telegravim_opaque_variants"_q, retained);
				}
			} else {
				auto &module = p->modules[conflict.moduleId];
				auto records = Records(module);
				if (conflict.recordId == u"@module-extra"_q) {
					module = metadata;
				} else {
					auto resolved = selected;
					if (*choice == ConflictChoice::KeepBoth) {
						resolved = conflict.local;
						auto other = conflict.incoming;
						auto variants = resolved.value(u"_telegravim_conflict_versions"_q).toArray();
						for (const auto &variant : other.value(u"_telegravim_conflict_versions"_q).toArray()) {
							if (!variants.contains(variant)) {
							variants.push_back(variant);
							}
						}
						other.remove(u"_telegravim_conflict_versions"_q);
						if (!variants.contains(other)) {
							variants.push_back(other);
						}
						resolved.insert(u"_telegravim_conflict_versions"_q, variants);
					}
					resolved.insert(u"version_vector"_q, JoinedVector(
						conflict.local.value(u"version_vector"_q).toObject(),
						conflict.incoming.value(u"version_vector"_q).toObject()));
					if (auto result = Advance(resolved, next.deviceId); !result) {
						return result;
					}
					records[conflict.recordId] = resolved;
				}
				SetRecords(module, records);
			}
		}
		auto receipts = next.extra.value(u"_telegravim_resolutions"_q).toArray();
		if (!receipts.contains(conflict.id)) {
			receipts.push_back(conflict.id);
		}
		next.extra.insert(u"_telegravim_resolutions"_q, receipts);
	}
	for (auto &profile : next.profiles) {
		for (auto &module : profile.modules) {
			if (auto result = NormalizeBindings(module, next.deviceId); !result) {
				return result;
			}
		}
	}
	if (auto result = _private->commit(std::move(next), cancel); !result) {
		return result;
	}
	crl::on_main([weak = weak_from_this()] {
		if (const auto strong = weak.lock()) {
			strong->_changes.fire({});
		}
	});
	return {};
}

Result Store::restoreConflict(
		const QString &archiveId,
		const QString &expectedGeneration,
		Cancel cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if (auto result = _private->load(cancel); !result) {
		return result;
	}
	if (_private->state.generation != expectedGeneration) {
		return Failure(Error::Stale, u"Local data changed; review the saved version again."_q);
	}
	auto found = std::optional<ArchivedConflict>();
	WalkArchives(_private->state, [&](ArchivedConflict value) {
		if (value.id == archiveId) {
			found = std::move(value);
		}
		return !found && !Cancelled(cancel);
	});
	if (Cancelled(cancel)) {
		return Cancellation();
	} else if (!found) {
		return Failure(Error::Stale, u"The saved version is no longer available."_q);
	}
	auto next = _private->state;
	const auto &conflict = *found;
	if (conflict.scope == ArchiveScope::Record) {
		auto &module = next.profiles[conflict.profileId].modules[conflict.moduleId];
		auto records = Records(module);
		auto restored = conflict.alternate;
		if (restored.value(u"id"_q).toString() != conflict.recordId) {
			return Failure(Error::Invalid, u"Saved version identity does not match."_q);
		}
		if (restored.value(u"kind"_q).toString() == u"binding"_q
			&& !restored.value(u"deleted"_q).toBool()) {
			const auto tagId = restored.value(u"tag_id"_q).toString();
			const auto tag = records.value(tagId);
			if (tag.value(u"kind"_q).toString() != u"tag"_q
				|| tag.value(u"deleted"_q).toBool()) {
				return Failure(Error::Conflict, u"Restore the tag before its message binding."_q);
			}
			const auto messageId = restored.value(u"message"_q)
				.toObject().value(u"message_id"_q);
			for (auto i = records.begin(); i != records.end(); ++i) {
				if (i.key() != conflict.recordId
					&& i->value(u"kind"_q).toString() == u"binding"_q
					&& !i->value(u"deleted"_q).toBool()
					&& i->value(u"tag_id"_q) == restored.value(u"tag_id"_q)
					&& i->value(u"message"_q).toObject().value(u"message_id"_q)
						== messageId) {
					return Failure(Error::Conflict, u"This tag is already assigned to the message."_q);
				}
			}
		}
		auto variants = QJsonArray();
		for (const auto &value : conflict.current.value(u"_telegravim_conflict_versions"_q).toArray()) {
			if (value != conflict.alternate && !variants.contains(value)) {
				variants.push_back(value);
			}
		}
		auto previous = conflict.current;
		previous.remove(u"_telegravim_conflict_versions"_q);
		if (!variants.contains(previous)) {
			variants.push_back(previous);
		}
		restored.insert(u"_telegravim_conflict_versions"_q, variants);
		restored.insert(u"version_vector"_q, JoinedVector(
			conflict.current.value(u"version_vector"_q).toObject(),
			conflict.alternate.value(u"version_vector"_q).toObject()));
		if (auto result = Advance(restored, next.deviceId); !result) {
			return result;
		}
		records[conflict.recordId] = restored;
		SetRecords(module, records);
	} else if (conflict.scope == ArchiveScope::Module) {
		auto &profile = next.profiles[conflict.profileId];
		profile.modules[conflict.moduleId] = conflict.alternate;
		auto variants = QJsonArray();
		for (const auto &value : next.extra.value(u"_telegravim_opaque_variants"_q).toArray()) {
			const auto object = value.toObject();
			if (object.value(u"module"_q) != conflict.alternate
				|| ReadAccount(object.value(u"account"_q).toObject()) != profile.account) {
				variants.push_back(value);
			}
		}
		const auto previous = QJsonObject{
			{ u"id"_q, Digest(Json(conflict.current)) },
			{ u"account"_q, AccountJson(profile.account) },
			{ u"module"_q, conflict.current },
		};
		if (!variants.contains(previous)) {
			variants.push_back(previous);
		}
		next.extra.insert(u"_telegravim_opaque_variants"_q, variants);
	} else {
		auto target = static_cast<QJsonObject*>(nullptr);
		switch (conflict.scope) {
		case ArchiveScope::RootExtra: target = &next.extra; break;
		case ArchiveScope::RootTransfer: target = &next.transfer; break;
		case ArchiveScope::ProfileExtra:
			target = &next.profiles[conflict.profileId].extra;
			break;
		case ArchiveScope::ProfileTransfer:
			target = &next.profiles[conflict.profileId].transfer;
			break;
		case ArchiveScope::ModuleExtra:
			target = &next.profiles[conflict.profileId].modules[conflict.moduleId];
			break;
		case ArchiveScope::AssetExtra:
			target = &next.assets[conflict.moduleId].extra;
			break;
		case ArchiveScope::AssetTransfer:
			target = &next.assets[conflict.moduleId].transfer;
			break;
		case ArchiveScope::Record:
		case ArchiveScope::Module:
			return Failure(Error::Invalid, u"Invalid saved version scope."_q);
		}
		Assert(target != nullptr);
		const auto selected = conflict.alternate.value(conflict.field);
		auto variants = QJsonArray();
		for (const auto &value : target->value(u"_telegravim_metadata_variants"_q).toArray()) {
			const auto object = value.toObject();
			if (object.value(u"field"_q).toString() != conflict.field
				|| object.value(u"value"_q) != selected) {
				variants.push_back(value);
			}
		}
		const auto previous = QJsonObject{
			{ u"field"_q, conflict.field },
			{ u"value"_q, target->value(conflict.field) },
		};
		if (!variants.contains(previous)) {
			variants.push_back(previous);
		}
		target->insert(conflict.field, selected);
		target->insert(u"_telegravim_metadata_variants"_q, variants);
	}
	for (auto &profile : next.profiles) {
		for (auto &module : profile.modules) {
			if (auto result = NormalizeBindings(module, next.deviceId); !result) {
				return result;
			}
		}
	}
	if (auto result = _private->commit(std::move(next), cancel); !result) {
		return result;
	}
	crl::on_main([weak = weak_from_this()] {
		if (const auto strong = weak.lock()) {
			strong->_changes.fire({});
		}
	});
	return {};
}

rpl::producer<> Store::changes() const {
	return _changes.events();
}

} // namespace Storage::LocalData
