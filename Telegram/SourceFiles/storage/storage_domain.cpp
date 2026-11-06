/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_domain.h"

#include "core/version.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/serialize_common.h"
#include "fakepasscode/actions/logout.h"
#include "mtproto/mtproto_config.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "base/openssl_help.h"
#include "base/random.h"
#include "core/version.h"
#include "config.h"

#include <QDir>

#include "fakepasscode/actions/clear_proxies.h"
#include "fakepasscode/fake_passcode.h"
#include "fakepasscode/log/fake_log.h"
#include "fakepasscode/autodelete/autodelete_service.h"
#include "fakepasscode/ptg.h"
#include "fakepasscode/settings.h"
#include "platform/platform_specific.h"
#include "storage/details/storage_fake_passcode_data.h"
#include "wallet/wallet_vault.h"
#include "tgw/tgw_storage.h"

namespace Storage {

struct PasscodeWrap final {
	details::PasscodeKdf kdf;
	quint32 generation = 0;
	QByteArray salt;
	QByteArray keyEncrypted;
};

// The two legacy flags travel together and are cleared together. legacy says
// the file carries only the three old blobs, so openKeyEncrypted is whatever
// the pre-change build wrote there; legacyPasscode says that blob is wrapped
// under a passcode rather than under the empty string. The parse cannot tell
// the two apart - both look like a non-empty openKeyEncrypted - so the second
// flag is set at the point the typed passcode proved which one it is, and it
// is what lets hasPasscode() and appLockEnabled() keep reporting the truth
// while a migration that failed to derive its wrap waits for the next start.
struct KeyData final {
	details::FakePasscodeData fake;
	bool ptg = false;
	QByteArray openSalt;
	QByteArray openKeyEncrypted;
	std::vector<PasscodeWrap> passcodeWraps;
	quint32 committed = 0;
	bool legacy = false;
	bool legacyPasscode = false;
	bool openKeyVerified = false;
};

namespace {

using namespace details;

enum class ReadKeyDataResult {
	Success,
	UnsupportedKdf,
	Failed,
};

// Upstream legacy profiles end after three blobs, while PTG profiles append
// an ASCII fake-wrap count and its encrypted blobs. The modern root-wrap
// trailer has its own magic, followed by a separate bounded PTG trailer.
// Its fake envelopes wrap the local key directly, so changing the real
// memory-hard passcode never requires retaining that passcode or its key.
// The encrypted account payload authenticates the fake count, and unlock
// resolves the fake identity against that payload before running actions.
constexpr auto kKeyDataMagic = quint32(0x4B443200);
constexpr auto kKeyDataFormatVersion = quint32(2);
constexpr auto kMaxWrapCount = quint32(2);

[[nodiscard]] QString BaseGlobalPath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString ComputeKeyName(const QString &dataName) {
	// We dropped old test authorizations when migrated to multi auth.
	//return "key_" + dataName + (cTestMode() ? "[test]" : "");
	return "key_" + dataName;
}

[[nodiscard]] QString ComputeWalletKeyringName(const QString &dataName) {
	return u"wallet_keyring_"_q + dataName;
}

[[nodiscard]] QByteArray RandomSalt() {
	auto result = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(result.data(), result.size());
	return result;
}

[[nodiscard]] bool SameWrapKey(const PasscodeWrap &a, const PasscodeWrap &b) {
	return (a.kdf.kind == b.kdf.kind)
		&& (a.kdf.memory == b.kdf.memory)
		&& (a.kdf.time == b.kdf.time)
		&& (a.kdf.parallel == b.kdf.parallel)
		&& (a.salt == b.salt);
}

[[nodiscard]] QByteArray WrapLocalKey(
		const MTP::AuthKeyPtr &localKey,
		const MTP::AuthKeyPtr &wrapKey) {
	Expects(localKey != nullptr);
	Expects(wrapKey != nullptr);

	EncryptedDescriptor data(MTP::AuthKey::kSize);
	localKey->write(data.stream);
	return PrepareEncrypted(data, wrapKey);
}

[[nodiscard]] MTP::AuthKeyPtr UnwrapLocalKey(
		const QByteArray &keyEncrypted,
		const MTP::AuthKeyPtr &wrapKey) {
	if (!wrapKey) {
		return nullptr;
	}
	EncryptedDescriptor data;
	if (!DecryptLocal(data, keyEncrypted, wrapKey)) {
		return nullptr;
	}
	auto key = Serialize::read<MTP::AuthKey::Data>(data.stream);
	if (data.stream.status() != QDataStream::Ok || !data.stream.atEnd()) {
		return nullptr;
	}
	return std::make_shared<MTP::AuthKey>(key);
}

[[nodiscard]] bool WrapOpensLocalKey(
		const QByteArray &keyEncrypted,
		const MTP::AuthKeyPtr &wrapKey,
		const MTP::AuthKeyPtr &localKey) {
	const auto recovered = UnwrapLocalKey(keyEncrypted, wrapKey);
	return recovered && recovered->equals(localKey);
}

[[nodiscard]] ReadKeyDataResult ReadKeyData(
		QDataStream &stream,
		KeyData &data,
		QByteArray &infoEncrypted) {
	stream >> data.openSalt >> data.openKeyEncrypted >> infoEncrypted;
	if (!CheckStreamStatus(stream)) {
		return ReadKeyDataResult::Failed;
	} else if (data.openSalt.size() != LocalEncryptSaltSize) {
		LOG(("App Error: bad salt in info file, size: %1"
			).arg(data.openSalt.size()));
		return ReadKeyDataResult::Failed;
	}
	if (stream.atEnd()) {
		data.legacy = true;
		return ReadKeyDataResult::Success;
	}
	auto trailer = QByteArray();
	stream >> trailer;
	if (!CheckStreamStatus(stream)) {
		return ReadKeyDataResult::Failed;
	}
	if (LegacyFakePasscodeCount(trailer)) {
		const auto fake = ReadLegacyFakePasscodeData(stream, trailer, data.openSalt);
		if (!fake) {
			return ReadKeyDataResult::Failed;
		}
		data.fake = *fake;
		data.ptg = true;
		data.legacy = true;
		return ReadKeyDataResult::Success;
	}
	QBuffer buffer(&trailer);
	if (!buffer.open(QIODevice::ReadOnly)) {
		LOG(("App Error: could not open the key data trailer."));
		return ReadKeyDataResult::Failed;
	}
	QDataStream inner(&buffer);
	inner.setVersion(QDataStream::Qt_5_1);

	auto magic = quint32();
	auto formatVersion = quint32();
	auto wrapCount = quint32();
	inner >> magic >> formatVersion >> data.committed >> wrapCount;
	if (!CheckStreamStatus(inner)) {
		return ReadKeyDataResult::Failed;
	} else if (magic != kKeyDataMagic) {
		LOG(("App Error: bad key data trailer magic."));
		return ReadKeyDataResult::Failed;
	} else if (formatVersion > kKeyDataFormatVersion) {
		LOG(("App Error: too new key data format: %1").arg(formatVersion));
		return ReadKeyDataResult::Failed;
	} else if (wrapCount > kMaxWrapCount) {
		LOG(("App Error: bad key data wrap count: %1").arg(wrapCount));
		return ReadKeyDataResult::Failed;
	}
	auto unsupportedCost = false;
	data.passcodeWraps.reserve(wrapCount);
	for (auto i = quint32(); i != wrapCount; ++i) {
		auto wrap = PasscodeWrap();
		inner >> wrap.kdf.kind
			>> wrap.generation
			>> wrap.kdf.memory
			>> wrap.kdf.time
			>> wrap.kdf.parallel
			>> wrap.salt
			>> wrap.keyEncrypted;
		if (!CheckStreamStatus(inner)) {
			return ReadKeyDataResult::Failed;
		} else if (!wrap.kdf.costWithinLimits()) {
			unsupportedCost = true;
		} else if (!wrap.kdf.valid()) {
			LOG(("App Error: bad key data KDF family: %1").arg(wrap.kdf.kind));
			return ReadKeyDataResult::Failed;
		}
		if (wrap.salt.size() < kPasscodeSaltMinSize) {
			LOG(("App Error: bad key data wrap salt size: %1"
				).arg(wrap.salt.size()));
			return ReadKeyDataResult::Failed;
		}
		for (const auto &already : data.passcodeWraps) {
			if (already.generation == wrap.generation) {
				LOG(("App Error: duplicate key data wrap generation: %1"
					).arg(wrap.generation));
				return ReadKeyDataResult::Failed;
			}
		}
		data.passcodeWraps.push_back(std::move(wrap));
	}
	const auto fake = ReadFakePasscodeData(stream);
	if (!fake) {
		return ReadKeyDataResult::Failed;
	}
	data.fake = *fake;
	data.ptg = !fake->salt.isEmpty();
	return unsupportedCost
		? ReadKeyDataResult::UnsupportedKdf
		: ReadKeyDataResult::Success;
}

void WriteKeyData(
		FileWriteDescriptor &file,
		const KeyData &data,
		const QByteArray &infoEncrypted) {
	file.writeData(data.openSalt);
	file.writeData(data.openKeyEncrypted);
	file.writeData(infoEncrypted);
	if (data.legacy) {
		file.writeData(QByteArray::number(data.fake.keys.size()));
		for (const auto &key : data.fake.keys) {
			file.writeData(key);
		}
		return;
	}
	auto trailer = QByteArray();
	QBuffer buffer(&trailer);
	const auto opened = buffer.open(QIODevice::WriteOnly);
	Assert(opened);
	QDataStream stream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kKeyDataMagic
		<< kKeyDataFormatVersion
		<< data.committed
		<< quint32(data.passcodeWraps.size());
	for (const auto &wrap : data.passcodeWraps) {
		stream
			<< wrap.kdf.kind
			<< wrap.generation
			<< wrap.kdf.memory
			<< wrap.kdf.time
			<< wrap.kdf.parallel
			<< wrap.salt
			<< wrap.keyEncrypted;
	}
	buffer.close();
	file.writeData(trailer);
	file.writeData(SerializeFakePasscodeData(data.fake));
}

} // namespace

PasscodeVerification::PasscodeVerification(quint64 nonce)
: _nonce(nonce) {
}

PasscodeDerivation::PasscodeDerivation(
	std::unique_ptr<PasscodeWrap> wrap,
	const QByteArray &passcode)
: _wrap(std::move(wrap))
, _passcode(passcode.constData(), passcode.size())
, _hardwareBound(PTG::IsHWLockEnabled()) {
}

PasscodeDerivation::PasscodeDerivation(PasscodeDerivation &&other) noexcept
: _wrap(base::take(other._wrap))
, _passcode(base::take(other._passcode))
, _key(base::take(other._key))
, _fakeSalt(base::take(other._fakeSalt))
, _fakeKey(base::take(other._fakeKey))
, _hardwareBound(other._hardwareBound)
, _cleansed(base::take(other._cleansed)) {
}

PasscodeDerivation &PasscodeDerivation::operator=(
		PasscodeDerivation &&other) noexcept {
	cleanse();
	_wrap = base::take(other._wrap);
	_passcode = base::take(other._passcode);
	_key = base::take(other._key);
	_fakeSalt = base::take(other._fakeSalt);
	_fakeKey = base::take(other._fakeKey);
	_hardwareBound = other._hardwareBound;
	_cleansed = base::take(other._cleansed);
	return *this;
}

PasscodeDerivation::~PasscodeDerivation() {
	cleanse();
}

void PasscodeDerivation::run() {
	if (!_fakeSalt.isEmpty() && !_passcode.isEmpty()) {
		_fakeKey = CreateLocalKey(_passcode, _fakeSalt, _hardwareBound);
	}
	if (!_wrap || _passcode.isEmpty()) {
		return;
	} else if (_wrap->kdf.kind == 0) {
		_key = CreateLocalKey(_passcode, _wrap->salt, _hardwareBound);
		return;
	}
	_key = CreatePasscodeKey(_hardwareBound
		? Platform::PTG::HWProtectPasscode(_passcode)
		: _passcode, _wrap->salt, _wrap->kdf);
	cleanse();
}

bool PasscodeDerivation::empty() const {
	return _passcode.isEmpty();
}

MTP::AuthKeyPtr PasscodeDerivation::keyFor(const PasscodeWrap &wrap) {
	const auto matched = _wrap && SameWrapKey(*_wrap, wrap);
	if (matched && _key) {
		return _key;
	} else if (_cleansed) {
		if (!matched) {
			LOG(("App Error: the passcode wrap changed under the derivation."));
		}
		return nullptr;
	} else if (wrap.kdf.kind == 0) {
		return CreateLocalKey(_passcode, wrap.salt, _hardwareBound);
	} else if (_passcode.isEmpty()) {
		return nullptr;
	}
	return CreatePasscodeKey(_hardwareBound
		? Platform::PTG::HWProtectPasscode(_passcode)
		: _passcode, wrap.salt, wrap.kdf);
}

void PasscodeDerivation::cleanse() {
	if (!_passcode.isEmpty()) {
		OPENSSL_cleanse(_passcode.data(), _passcode.size());
	}
	_cleansed = true;
}

Domain::Domain(not_null<Main::Domain*> owner, const QString &dataName)
: _owner(owner)
, _dataName(dataName)
, _keyData(std::make_unique<KeyData>()) {
}

Domain::~Domain() = default;

StartResult Domain::start(PasscodeDerivation derived) {
    PTG::SetHWLockEnabled(false);
	auto modern = startModern(derived);
	if (modern == StartModernResult::IncorrectPasscode
		&& _keyData->legacy
		&& !derived.empty()
		&& !derived._cleansed
		&& Platform::PTG::IsHWProtectionAvailable()) {
            PTG::SetHWLockEnabled(true);
		derived._hardwareBound = true;
		derived._key = nullptr;
		derived._fakeKey = nullptr;
		modern = startModern(derived);
    }
	if (modern == StartModernResult::Success) {
		if (_keyData->legacy || _keyDataDirty || _oldVersion < AppVersion) {
			writeAccounts();
            if (_oldVersion == FakeAppVersion) {
                _oldVersion = AppVersion;
            }
		}
		return StartResult::Success;
	} else if (modern == StartModernResult::IncorrectPasscode) {
		return StartResult::IncorrectPasscode;
	} else if (modern == StartModernResult::Failed) {
		// startFromScratch() drops a local key startModern() already read.
		startFromScratch();
		return StartResult::Success;
	}
	auto legacy = std::make_unique<Main::Account>(_owner, _dataName, 0);
	const auto result = legacy->legacyStart(derived._passcode);
	if (result == StartResult::Success) {
		_oldVersion = legacy->local().oldMapVersion();
		startWithSingleAccount(derived._passcode, std::move(legacy));
	}
	return result;
}

void Domain::startAdded(
		not_null<Main::Account*> account,
		std::unique_ptr<MTP::Config> config) {
	Expects(_localKey != nullptr);

	account->prepareToStartAdded(_localKey);
	account->start(std::move(config));
}

void Domain::startWithSingleAccount(
		const QByteArray &passcode,
		std::unique_ptr<Main::Account> account) {
	Expects(account != nullptr);

	if (auto localKey = account->local().peekLegacyLocalKey()) {
		_localKey = std::move(localKey);
		installLegacyWrap(*_keyData, passcode);
		_keyData->fake.salt = _keyData->openSalt;
		_passcodeKeySalt = _keyData->fake.salt;
		migrateFromLegacy(passcode);
		account->start(nullptr);
	} else {
		generateLocalKey();
		account->start(account->prepareToStart(_localKey));
	}
	_owner->accountAddedInStorage(Main::Domain::AccountWithIndex{
		.account = std::move(account)
	});
	writeAccounts();
}

void Domain::generateLocalKey() {
	Expects(_localKey == nullptr);
	Expects(_keyData->openKeyEncrypted.isEmpty());
	Expects(_keyData->passcodeWraps.empty());

	auto pass = QByteArray(MTP::AuthKey::kSize, Qt::Uninitialized);
	base::RandomFill(pass.data(), pass.size());
	const auto salt = RandomSalt();
	_localKey = CreateLocalKey(pass, salt);

	installOpenWrap(*_keyData);
	_keyData->fake.salt = RandomSalt();
	_passcodeKeySalt = _keyData->fake.salt;
}

void Domain::installOpenWrap(KeyData &data) const {
	Expects(_localKey != nullptr);

	data.openSalt = RandomSalt();
	const auto wrapKey = CreateLocalKey(QByteArray(), data.openSalt, data.fake.hardwareBound);
	data.openKeyEncrypted = WrapLocalKey(_localKey, wrapKey);
	data.openKeyVerified = WrapOpensLocalKey(
		data.openKeyEncrypted,
		wrapKey,
		_localKey);
}

// Turning the app lock on drops the open wrap, recording its absence with
// an empty openKeyEncrypted - never with an empty openSalt. A build of the
// same AppVersion that predates this format reads only the three legacy
// blobs: a real-size salt beside an empty wrap makes it fail inside
// DecryptLocal and ask for a passcode, while an empty salt would make it fail
// the salt-size check, start from scratch and orphan every account store. So
// the salt stays 32 real random bytes in both states.
void Domain::dropOpenWrap(KeyData &data) const {
	data.openSalt = RandomSalt();
	data.openKeyEncrypted = QByteArray();
	data.openKeyVerified = false;
}

void Domain::installLegacyWrap(
		KeyData &data,
		const QByteArray &passcode) const {
	Expects(_localKey != nullptr);

	data.openSalt = RandomSalt();
	const auto wrapKey = CreateLocalKey(passcode, data.openSalt);
	data.openKeyEncrypted = WrapLocalKey(_localKey, wrapKey);
	data.legacy = true;
	data.legacyPasscode = !passcode.isEmpty();
	data.openKeyVerified = passcode.isEmpty()
		&& WrapOpensLocalKey(data.openKeyEncrypted, wrapKey, _localKey);
}

MTP::AuthKeyPtr Domain::installPasscodeWrap(
		KeyData &data,
		PasscodeDerivation &derived,
		quint32 generation) const {
	Expects(_localKey != nullptr);
	Expects(!derived.empty());
	Expects(derived._wrap && derived._wrap->keyEncrypted.isEmpty());

	auto wrap = *derived._wrap;
	wrap.generation = generation;
	const auto wrapKey = derived.keyFor(wrap);
	if (!wrapKey) {
		LOG(("App Error: could not derive the passcode key, family: %1"
			).arg(wrap.kdf.kind));
		return nullptr;
	}
	wrap.keyEncrypted = WrapLocalKey(_localKey, wrapKey);
	if (!WrapOpensLocalKey(wrap.keyEncrypted, wrapKey, _localKey)) {
		LOG(("App Error: a fresh passcode wrap does not open the local key."));
		return nullptr;
	}
	data.passcodeWraps.push_back(std::move(wrap));
	return wrapKey;
}

// Migration keeps "a passcode means the app lock is on", so hasLocalPasscode(),
// the auto-lock and the lock icon read exactly as they did before it ran. A
// derivation failure is not allowed to lose either the data or the app lock:
// the key data is then left in the legacy shape it was read in, which writes
// back byte for byte what the pre-change build wrote, and the next start
// simply tries the migration again. In that state openKeyEncrypted still holds
// the passcode-derived legacy blob, which legacyPasscode records, so the
// predicates keep reporting a passcode with the app lock armed and every
// mutator refuses instead of relabelling that blob as an open wrap.
void Domain::migrateFromLegacy(const QByteArray &passcode) {
	Expects(_keyData->legacy);
	Expects(_localKey != nullptr);

	if (passcode.isEmpty()) {
		installOpenWrap(*_keyData);
		_keyData->passcodeWraps.clear();
		_keyData->committed = 0;
	} else {
		auto fresh = prepareNewWrap(passcode);
		if (!installPasscodeWrap(*_keyData, fresh, 1)) {
			return;
		}
		dropOpenWrap(*_keyData);
		_keyData->committed = 1;
	}
	_keyData->legacy = false;
	_keyData->legacyPasscode = false;
	_keyDataDirty = true;
	_verificationNonce = 0;
}

void Domain::encryptLocalKey(const QByteArray &passcode) {
	if (!_keyData->legacy) {
		Expects(passcode.isEmpty());
		_keyData->passcodeWraps.clear();
		++_keyData->committed;
		installOpenWrap(*_keyData);
		_hasLocalPasscode = false;
		if (_autoDelete) {
			_autoDelete->DeleteAll();
		}
		return;
	}
	_passcodeKeySalt.resize(LocalEncryptSaltSize);
	base::RandomFill(_passcodeKeySalt.data(), _passcodeKeySalt.size());
	_passcodeKey = CreateLocalKey(passcode, _passcodeKeySalt);
	EncryptedDescriptor passKeyData(MTP::AuthKey::kSize);
	_localKey->write(passKeyData.stream);
	_passcodeKeyEncrypted = PrepareEncrypted(passKeyData, _passcodeKey);
	_hasLocalPasscode = !passcode.isEmpty();
	_keyData->openSalt = _passcodeKeySalt;
	_keyData->openKeyEncrypted = _passcodeKeyEncrypted;
	_keyData->legacyPasscode = _hasLocalPasscode;
	_keyData->fake.salt = _passcodeKeySalt;
    if (!_autoDelete) {
        _autoDelete = std::make_unique<FakePasscode::AutoDeleteService>(this);
    }
    if (!_hasLocalPasscode && _autoDelete) {
        //can't store data anymore, so delete all
        _autoDelete->DeleteAll();
    }
}

Domain::StartModernResult Domain::startModern(
		PasscodeDerivation &derived) {
	const auto name = ComputeKeyName(_dataName);

	FileReadDescriptor file;
	if (!ReadFile(file, name, BaseGlobalPath())) {
		return StartModernResult::Empty;
	}
        LOG(("App Info: reading accounts info..."));

	auto parsed = KeyData();
	auto infoEncrypted = QByteArray();
	const auto read = ReadKeyData(file.stream, parsed, infoEncrypted);
	if (read == ReadKeyDataResult::Failed) {
		return StartModernResult::IncorrectPasscode;
	} else if (read == ReadKeyDataResult::UnsupportedKdf) {
		return StartModernResult::IncorrectPasscode;
	}
	*_keyData = std::move(parsed);
	_localKey = nullptr;
	if (!_keyData->legacy) {
		PTG::SetHWLockEnabled(_keyData->fake.hardwareBound);
	}
	_fakePasscodeIndex = -1;
	_isStartedWithFake = false;
	if (_keyData->fake.salt.isEmpty()) {
		_keyData->fake.salt = _keyData->openSalt;
		_keyDataDirty = true;
	}
	_passcodeKeySalt = _keyData->fake.salt;
	_fakePasscodeKeysEncrypted = _keyData->fake.keys;
	_fakePasscodes.resize(_fakePasscodeKeysEncrypted.size());

	// Probe the stored open wrap independently of the typed passcode result.
	// PrepareEncrypted adds a 16-byte prefix to the padded length and key.
	// Only this ciphertext size can hold the fixed-size local key. Bound
	// this extra decrypt before it allocates, keeping the selected unwrap
	// and startup results intact. Compare once that unwrap gives a key.
	constexpr auto kOpenWrapSize = 16
		+ ((sizeof(uint32) + MTP::AuthKey::kSize + 15) / 16) * 16;
	auto openKey = (_keyData->openKeyEncrypted.size() == kOpenWrapSize)
		? UnwrapLocalKey(
			_keyData->openKeyEncrypted,
			CreateLocalKey(QByteArray(), _keyData->openSalt))
		: nullptr;

	auto wrapKey = MTP::AuthKeyPtr();
	auto localKeyEncrypted = QByteArray();
	if (_keyData->legacy) {
		const auto blob = PasscodeWrap{
			.salt = _keyData->openSalt,
			.keyEncrypted = _keyData->openKeyEncrypted,
		};
		wrapKey = derived.keyFor(blob);
		if (!wrapKey) {
			return StartModernResult::IncorrectPasscode;
		}
		localKeyEncrypted = blob.keyEncrypted;
	} else {
		// A passcode wrap is live only while its generation equals the
		// committed one. That drops a staged wrap left above committed by a
		// crash in the middle of a passcode change and a stale wrap left
		// below it, and it is also what turns an interrupted creation of a
		// first passcode back into the "no passcode" file it was before.
		auto live = std::vector<PasscodeWrap>();
		for (auto &wrap : _keyData->passcodeWraps) {
			if (wrap.generation == _keyData->committed) {
				live.push_back(std::move(wrap));
			}
		}
		if (live.size() != _keyData->passcodeWraps.size()) {
			_keyDataDirty = true;
		}
		_keyData->passcodeWraps = std::move(live);

		if (derived.empty()) {
			if (_keyData->openKeyEncrypted.isEmpty()) {
				LOG(("App Info: the app lock is on, a passcode is needed."));
				return StartModernResult::IncorrectPasscode;
			}
			wrapKey = CreateLocalKey(QByteArray(), _keyData->openSalt);
			localKeyEncrypted = _keyData->openKeyEncrypted;
		} else if (_keyData->passcodeWraps.size() != 1) {
			LOG(("App Info: no passcode wrap to open the local key with."));
			return StartModernResult::IncorrectPasscode;
		} else {
			const auto &wrap = _keyData->passcodeWraps.front();
			wrapKey = derived.keyFor(wrap);
			if (!wrapKey) {
				LOG(("App Error: passcode KDF family %1 is unavailable."
					).arg(wrap.kdf.kind));
				return StartModernResult::IncorrectPasscode;
			}
			localKeyEncrypted = wrap.keyEncrypted;
		}
	}

	EncryptedDescriptor keyInnerData;
	if (!DecryptLocal(keyInnerData, localKeyEncrypted, wrapKey)) {
		const auto fakeKey = derived._fakeKey
			? derived._fakeKey
			: (!derived._cleansed && !derived._passcode.isEmpty())
			? CreateLocalKey(derived._passcode, _passcodeKeySalt, derived._hardwareBound)
			: nullptr;
		if (!fakeKey || _fakePasscodeKeysEncrypted.empty()) {
			return StartModernResult::IncorrectPasscode;
	}
		_passcodeKey = fakeKey;
		_fakeUnlockKey = fakeKey;
		_keyData->fake.hardwareBound = PTG::IsHWLockEnabled();
		_keyData->legacyPasscode = _keyData->legacy;
		_oldVersion = file.version;
		const auto result = tryFakeStart(localKeyEncrypted, infoEncrypted, _passcodeKeySalt, {});
		if (result == StartModernResult::Success) {
			_keyData->openKeyVerified = openKey && openKey->equals(_localKey);
		}
		return result;
	}
	_passcodeKey = wrapKey;
	_oldVersion = file.version;
	const auto result = startUsingKeyStream(keyInnerData, localKeyEncrypted,
		infoEncrypted, _passcodeKeySalt, !derived.empty());
	if (result != StartModernResult::Success) {
		_localKey = nullptr;
		return StartModernResult::IncorrectPasscode;
	}
	_keyData->openKeyVerified = openKey && openKey->equals(_localKey);
	if (_keyData->legacy) {
		_keyData->legacyPasscode = !derived.empty();
		_keyData->fake.hardwareBound = PTG::IsHWLockEnabled();
		migrateFromLegacy(derived._passcode);
	}
	return StartModernResult::Success;
}

void Domain::writeAccounts() {
	Expects(!_owner->accounts().empty());
	Expects(!_keyData->openKeyEncrypted.isEmpty()
		|| !_keyData->passcodeWraps.empty());
	Expects(_keyData->passcodeWraps.size() <= 1);

	writeKeyData(*_keyData, false);
	_keyDataDirty = false;
}

QByteArray Domain::prepareAccountsInfo() const {
	Expects(_localKey != nullptr);
	const auto &list = _owner->accounts();

    qint32 serializedSize = 0;
    std::vector<QByteArray> serializedActions;
    std::vector<QByteArray> fakePasscodes;
    std::vector<QByteArray> fakeNames;
    QByteArray autoDeleteData;

    FAKE_LOG(qsl("Serialize fake passcodes"));
    for (const auto& fakePasscode : _fakePasscodes) {
        QByteArray curSerializedActions = fakePasscode.SerializeActions();
        auto fakePass = fakePasscode.GetPasscode();
        QByteArray name = fakePasscode.GetCurrentName().toUtf8();
        serializedSize += curSerializedActions.size() + fakePass.size() + name.size();
        serializedActions.push_back(std::move(curSerializedActions));
        fakePasscodes.push_back(std::move(fakePass));
        fakeNames.push_back(std::move(name));
    }
    FAKE_LOG(qsl("Serialize auto delete"));
    if (_autoDelete) {
        autoDeleteData = _autoDelete->Serialize();
    }

	auto keySize = sizeof(qint32) + sizeof(qint32) * list.size() + 3 * sizeof(bool) + sizeof(qint32);

    if (!_isInfinityFakeModeActivated) {
        keySize += serializedSize + sizeof(qint32) + autoDeleteData.size();
    }

    FAKE_LOG(qsl("Key size: %1").arg(keySize));
	EncryptedDescriptor keyData(keySize);
    std::vector<qint32> account_indexes;
    std::vector<qint32> full_account_indexes;
    account_indexes.reserve(list.size());
    std::set<qint32> account_hidden_indexes;

    // collect hidden accounts
    for (const auto& fakePasscode : _fakePasscodes) {
        if (fakePasscode.ContainsAction(FakePasscode::ActionType::Logout)) {
            const auto *logout = dynamic_cast<const FakePasscode::LogoutAction *>(
                fakePasscode[FakePasscode::ActionType::Logout]
            );
            for (const auto& index : logout->GetAccounts()) {
                account_hidden_indexes.insert(index);
            }
        }
    }

    FAKE_LOG(qsl("Enumerate accounts for logout"));
	for (const auto &[index, account] : list) {
        full_account_indexes.push_back(index);
        if (account_hidden_indexes.find(index) != account_hidden_indexes.end()) {
            FAKE_LOG(qsl("We have account %1 in logout or hide action. Skip").arg(index));
            continue;
	}
        account_indexes.push_back(index);
	}

    FAKE_LOG(qsl("%1 accounts left").arg(account_indexes.size()));
    keyData.stream << qint32(account_indexes.size());

    for (qint32 index : account_indexes) {
        FAKE_LOG(qsl("Save account %1 to main storage").arg(index));
        keyData.stream << index;
    }

    keyData.stream << qint32(_owner->activeForStorage());
    keyData.stream << _isInfinityFakeModeActivated;
    FAKE_LOG(qsl("Write accounts with infinite mode activate?: %1").arg(_isInfinityFakeModeActivated));
    if (!_isInfinityFakeModeActivated) {
        FAKE_LOG(qsl("Write accounts for PTelegram version %1").arg(PTelegramAppVersion));
        keyData.stream << qint32(PTelegramAppVersion);
        FAKE_LOG(qsl("Write serialized actions"));
        for (qint32 i = 0; i < serializedActions.size(); ++i) {
            keyData.stream << serializedActions[i];
            keyData.stream << fakePasscodes[i];
            keyData.stream << fakeNames[i];
        }

        FAKE_LOG(qsl("Write flags"));
        keyData.stream << _isCacheCleanedUpOnLock;
        keyData.stream << _isAdvancedLoggingEnabled;
        keyData.stream << _isErasingEnabled;

        FAKE_LOG(qsl("Write auto delete"));
        keyData.stream << autoDeleteData;

        keyData.stream << _cacheFolderPermissionRequested;

        // Added 1.7.0
        FAKE_LOG(qsl("Write actual account list"));
        keyData.stream << qint32(full_account_indexes.size());
        for (qint32 index : full_account_indexes) {
            FAKE_LOG(qsl("Save account %1 to main storage").arg(index));
            keyData.stream << index;
        }

        // Added 1.8.4
        PTG::DASettings::serialize(keyData.stream);

        // Added PTG 2.0
        PTG::serialize(keyData.stream);

    }

	AppendFakePasscodeCount(keyData.stream,
		_isInfinityFakeModeActivated ? 0 : int(_fakePasscodes.size()));
	return PrepareEncrypted(keyData, _localKey);
}

bool Domain::writeKeyData(const KeyData &data, bool sync) const {
	const auto path = BaseGlobalPath();
	if (!QDir().exists(path)) {
		QDir().mkpath(path);
	}

	FileWriteDescriptor key(ComputeKeyName(_dataName), path, sync);
	auto written = data;
	written.fake.salt = _passcodeKeySalt;
	written.fake.keys = fakeKeyWraps(data.legacy, data.fake.hardwareBound);
	WriteKeyData(key, written, prepareAccountsInfo());
	return key.finish();
}

// Unlike writeAccounts(), which queues the bytes and always reports success,
// this one commits them on the storage thread and answers whether they really
// reached the disk. Every path that changes which secret opens the local key
// goes through it, because a removal or a rewrap that only looked like it was
// written would leave the user with a file none of the secrets they know can
// open. details::Sync() is deliberately not called afterwards: the sync
// descriptor already drops any queued entry for the same base and performs the
// write before finish() returns.
bool Domain::writeKeyDataChecked(const KeyData &data) const {
	Expects(!_owner->accounts().empty());
	Expects(!data.legacy);
	Expects(!data.openKeyEncrypted.isEmpty() || !data.passcodeWraps.empty());
	Expects(data.passcodeWraps.size() <= kMaxWrapCount);

	const auto written = writeKeyData(data, true);
	if (!written) {
		LOG(("App Error: could not write the accounts key file."));
    }
	return written;
}

// The staged wrap was already proved to open the local key in memory, so what
// is left to prove is that the bytes the write path put on disk are those same
// bytes. Comparing the parsed wrap field by field against the staged one and
// then unwrapping with the key the derivation already produced keeps the whole
// read-back round trip - a serialization that dropped, reordered or truncated
// a field still fails here - while paying for the memory-hard derivation once
// instead of twice: byte-equal salt and parameters can only re-derive the same
// key, so re-deriving it would prove nothing the comparison has not.
bool Domain::wrapOnDiskOpensLocalKey(
		const PasscodeWrap &staged,
		const MTP::AuthKeyPtr &wrapKey) const {
	Expects(_localKey != nullptr);
	Expects(wrapKey != nullptr);

	FileReadDescriptor file;
	if (!ReadFile(file, ComputeKeyName(_dataName), BaseGlobalPath())) {
		return false;
	}
	auto parsed = KeyData();
	auto infoEncrypted = QByteArray();
	const auto read = ReadKeyData(file.stream, parsed, infoEncrypted);
	if (read != ReadKeyDataResult::Success || parsed.legacy) {
		return false;
	}
	for (const auto &wrap : parsed.passcodeWraps) {
		if (wrap.generation != staged.generation) {
			continue;
		} else if (!SameWrapKey(wrap, staged)
			|| wrap.keyEncrypted != staged.keyEncrypted) {
			LOG(("App Error: the staged passcode wrap changed on disk."));
			return false;
		}
		return WrapOpensLocalKey(wrap.keyEncrypted, wrapKey, _localKey);
	}
	return false;
}

WalletEngineValue Domain::readWalletKeyring() const {
	using State = WalletEngineValue::State;
	if (IsFake()) {
		return { .state = State::Absent };
	}
	const auto name = ComputeWalletKeyringName(_dataName);
	const auto path = BaseGlobalPath();
	const auto base = path + name;
	if (!QFileInfo::exists(base + 's')
		&& !QFileInfo::exists(base + '0')
		&& !QFileInfo::exists(base + '1')) {
		return { .state = State::Absent };
	} else if (!_localKey) {
		return { .state = State::Broken };
	}
	FileReadDescriptor file;
	if (!ReadEncryptedFile(file, name, path, _localKey)) {
		return { .state = State::Broken };
	}
	auto bytes = QByteArray();
	file.stream >> bytes;
	if (!CheckStreamStatus(file.stream) || !file.stream.atEnd()) {
		return { .state = State::Broken };
	}
	return { .state = State::Read, .bytes = std::move(bytes) };
}

bool Domain::writeWalletKeyring(const QByteArray &bytes) const {
	if (IsFake()) {
		return false;
	}
	Expects(_localKey != nullptr);

	const auto path = BaseGlobalPath();
	if (!QDir().exists(path) && !QDir().mkpath(path)) {
		return false;
	}
	EncryptedDescriptor data(Serialize::bytearraySize(bytes));
	data.stream << bytes;
	FileWriteDescriptor file(ComputeWalletKeyringName(_dataName), path, true);
	file.writeEncrypted(data, _localKey);
	const auto written = file.finish();
	if (!written) {
		LOG(("Wallet Error: could not write the device keyring."));
	}
	return written;
}

bool Domain::removeWalletKeyring() {
	const auto base = BaseGlobalPath() + ComputeWalletKeyringName(_dataName);
	auto result = true;
	for (const auto suffix : { 's', '0', '1' }) {
		const auto path = base + suffix;
		if (QFileInfo::exists(path) && !QFile::remove(path)) {
			result = false;
		}
	}
	return result;
}

void Domain::startFromScratch() {
	if (!removeWalletKeyring()) {
		LOG(("Wallet Error: could not remove the discarded device keyring."));
	}
	Tgw::EraseDomainStorage(_owner, _dataName);
	*_keyData = KeyData();
	_keyDataDirty = false;
	_verificationNonce = 0;
	_localKey = nullptr;
	startWithSingleAccount(
		QByteArray(),
		std::make_unique<Main::Account>(_owner, _dataName, 0));
}

std::unique_ptr<PasscodeWrap> Domain::wrapToOpen() const {
	if (_keyData->legacy || _keyData->legacyPasscode) {
		return std::make_unique<PasscodeWrap>(PasscodeWrap{
			.salt = _keyData->openSalt,
			.keyEncrypted = _keyData->openKeyEncrypted,
		});
	} else if (_keyData->passcodeWraps.size() != 1) {
		return nullptr;
	}
	return std::make_unique<PasscodeWrap>(_keyData->passcodeWraps.front());
}

PasscodeDerivation Domain::prepareOpen(const QByteArray &passcode) const {
	auto result = PasscodeDerivation(wrapToOpen(), passcode);
	result._hardwareBound = _keyData->fake.hardwareBound;
	if (!_fakePasscodeKeysEncrypted.empty() || !_fakePasscodes.empty()) {
		result._fakeSalt = _passcodeKeySalt;
	}
	return result;
}

PasscodeDerivation Domain::prepareNewWrap(const QByteArray &passcode) const {
	if (IsFakeWithoutInfinityFlag()) {
		return PasscodeDerivation(std::make_unique<PasscodeWrap>(PasscodeWrap{
			.salt = _passcodeKeySalt,
		}), passcode);
	}
	return PasscodeDerivation(
		std::make_unique<PasscodeWrap>(PasscodeWrap{
			.kdf = DefaultPasscodeKdf(),
			.salt = RandomSalt(),
		}),
		passcode);
}

// The only decisions this makes before the derivation runs are which wrap the
// passcode has to open and whether such a wrap exists at all; neither depends
// on what was typed and so neither tells an attacker anything about it. From
// there a wrong passcode and the right one walk the same derivation and the
// same unwrap attempt, and the verdict is the comparison of the recovered key
// with the one already in memory - the typed passcode is never needed to open
// the file at start, only to prove it is the one the wrap was made from.
//
// The retained legacy state carries no passcode wrap: the only secret that
// opens the local key there is the passcode-derived blob the pre-change build
// left in openKeyEncrypted, so it is checked exactly the way startModern()
// opens it. That state reports a passcode with the app lock armed, and this
// leg is what lets the lock it arms be opened by the passcode that armed it.
bool Domain::checkPasscode(PasscodeDerivation derived) const {
	Expects(_localKey != nullptr);

	const auto wrap = wrapToOpen();
	if (!wrap) {
		return false;
	}
	const auto key = derived.keyFor(*wrap);
	return key && WrapOpensLocalKey(wrap->keyEncrypted, key, _localKey);
}

std::optional<PasscodeVerification> Domain::verifyPasscode(
		const QByteArray &passcode) {
	return verifyPasscode(prepareOpen(passcode));
}

std::optional<PasscodeVerification> Domain::verifyPasscode(
		PasscodeDerivation derived) {
	const auto fakeKey = derived._fakeKey
		? derived._fakeKey
		: (!derived._cleansed && !derived._passcode.isEmpty())
		? CreateLocalKey(derived._passcode, _passcodeKeySalt, derived._hardwareBound)
		: nullptr;
	const auto correct = IsFakeWithoutInfinityFlag()
		? (fakeKey && fakeKey->equals(_fakePasscodes[_fakePasscodeIndex].GetFakePasscodeKey()))
		: checkPasscode(std::move(derived));
	if (!correct) {
		return std::nullopt;
	}
	auto nonce = quint64();
	while (!nonce) {
		nonce = base::RandomValue<quint64>();
	}
	_verificationNonce = nonce;
	_verificationFakeIndex = _fakePasscodeIndex;
	_verificationInfinity = _isInfinityFakeModeActivated;
	return PasscodeVerification(nonce);
}

bool Domain::accepts(PasscodeVerification verification) const {
	return _verificationNonce
		&& (verification._nonce == _verificationNonce)
		&& (_verificationFakeIndex == _fakePasscodeIndex)
		&& (_verificationInfinity == _isInfinityFakeModeActivated);
}

SetPasscodeResult Domain::setPasscode(
		const QByteArray &passcode,
		PasscodeVerification verification) {
	if (IsFakeWithoutInfinityFlag()) {
		const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });
		if (!accepts(verification)) {
			return SetPasscodeResult::NeedsVerification;
		}
		_verificationNonce = 0;
		setFakePasscode(passcode);
		return SetPasscodeResult::Success;
	}
	if (passcode.isEmpty()) {
		return changePasscode(nullptr, verification);
	}
	auto derived = prepareNewWrap(passcode);
	return changePasscode(&derived, verification);
}

void Domain::setFakePasscode(const QByteArray &passcode) {
	Expects(!_passcodeKeySalt.isEmpty());
	Expects(_localKey != nullptr);
    FAKE_LOG(("Got new passcode"));
    if (IsFakeWithoutInfinityFlag()) {
        if (!passcode.isEmpty()) {
            FAKE_LOG(qsl("It goes to fake passcode %1").arg(_fakePasscodeIndex));
            _fakePasscodes[_fakePasscodeIndex].SetPasscode(passcode);
        } else if (_owner->maybeLastOrSomeAuthedAccount() == nullptr) {
            // no accounts - just reset to empty pass
            encryptLocalKey(passcode);
        } else {
            FAKE_LOG(("Infinity mode activated"));
            _isInfinityFakeModeActivated = true;
            encryptLocalKey(passcode);
            // clear all sensitive
            if (_autoDelete) {
                _autoDelete->DeleteAll();
            }
            _fakePasscodes[_fakePasscodeIndex].OnEvent(FakePasscode::ActionEvent::SwitchToInfinityFake);
        }
    } else {
        encryptLocalKey(passcode);
        PrepareEncryptedFakePasscodes(); // Since salt was changed
    }

    if (passcode.isEmpty()) {
        FAKE_LOG(qsl("Clear fake state"));
        ClearFakeState();
    }
    FAKE_LOG(qsl("Call write accounts from setPasscode"));
	writeAccounts();

	_passcodeKeyChanged.fire({});
}

SetPasscodeResult Domain::setPasscode(
		PasscodeDerivation derived,
		PasscodeVerification verification) {
	if (IsFakeWithoutInfinityFlag()) {
		const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });
		if (derived._cleansed || !accepts(verification)) {
			return SetPasscodeResult::NeedsVerification;
		}
		_verificationNonce = 0;
		setFakePasscode(derived._passcode);
		return SetPasscodeResult::Success;
	}
	Expects(!derived.empty());

	return changePasscode(&derived, verification);
}

// A passcode change is staged so that no crash window can leave a file only a
// passcode nobody has typed can open. The new wrap is written beside the old
// one first, with the committed generation left where it was, so a start that
// happens in that window drops the staged wrap by generation and finds exactly
// the previous truth; the bytes are then read back from disk and proved to
// open the local key, and only after that does a second write strip the old
// wrap and move committed forward. No write on this path ever adds a fresh
// open wrap, because a file carrying both the old passcode wrap and a fresh
// open wrap would keep the old passcode working while silently turning the app
// lock off - which no reconciliation rule at the next start can tell from an
// intended state. A removal is the mirror of the same rule and needs no
// staging: nothing new has to be proved, so it is a single atomic checked
// write of the open wrap with no passcode wrap left.
//
// This is the passcode-role mutator and it does not move the app-lock role.
// The open wrap is dropped only where setPasscode() creates a first passcode,
// the one case that has to keep the Settings behaviour of "setting a passcode
// locks the app"; createPasscodeWithoutAppLock() runs the same staged
// sequence for a first passcode in the wallet-only role and keeps the
// verified open wrap it already has, and a passcode change carries whatever
// open-wrap state the file already had straight through. A file still in the
// legacy shape is refused outright, because its openKeyEncrypted may be the
// passcode-derived blob and nothing here could honestly relabel that as an
// open wrap.
SetPasscodeResult Domain::changePasscode(
		PasscodeDerivation *derived,
		PasscodeVerification verification) {
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	if (_keyData->legacy) {
		LOG(("App Error: refusing a passcode change before the migration."));
		return SetPasscodeResult::Failed;
	} else if (!_keyData->passcodeWraps.empty() && !accepts(verification)) {
		return SetPasscodeResult::NeedsVerification;
	} else if (derived) {
		return installPasscode(*derived, !_keyData->passcodeWraps.empty());
	}
	auto updated = *_keyData;
	updated.passcodeWraps.clear();
	updated.committed = _keyData->committed + 1;
	installOpenWrap(updated);

	Assert(!updated.openKeyEncrypted.isEmpty());
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	ClearFakeState();
	if (_autoDelete) {
		_autoDelete->DeleteAll();
	}
	writeAccounts();
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

SetPasscodeResult Domain::installPasscode(
		PasscodeDerivation &derived,
		bool keepOpenWrap) {
	const auto generation = _keyData->committed + 1;
	auto staged = *_keyData;
	const auto wrapKey = installPasscodeWrap(staged, derived, generation);
	if (!wrapKey) {
		return SetPasscodeResult::Failed;
	} else if (!writeKeyDataChecked(staged)) {
		return SetPasscodeResult::Failed;
	} else if (!wrapOnDiskOpensLocalKey(
			staged.passcodeWraps.back(),
			wrapKey)) {
		if (!writeKeyDataChecked(*_keyData)) {
			LOG(("App Error: could not drop the staged passcode wrap."));
		}
		return SetPasscodeResult::Failed;
	}
	auto updated = *_keyData;
	updated.passcodeWraps.clear();
	updated.passcodeWraps.push_back(staged.passcodeWraps.back());
	updated.committed = generation;
	updated.fake.hardwareBound = PTG::IsHWLockEnabled();
	if (keepOpenWrap && _keyData->openKeyVerified
		&& updated.fake.hardwareBound != _keyData->fake.hardwareBound) {
		installOpenWrap(updated);
	}
	if (!keepOpenWrap) {
		dropOpenWrap(updated);
	}

	Assert(!updated.passcodeWraps.empty());
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	PrepareEncryptedFakePasscodes();
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

SetPasscodeResult Domain::createPasscodeWithoutAppLock(
		PasscodeDerivation derived) {
	if (IsFake()) {
		_verificationNonce = 0;
		return SetPasscodeResult::Failed;
	}
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	if (_keyData->legacy || _keyData->legacyPasscode) {
		LOG(("App Error: refusing a passcode without the app lock "
			"before the migration."));
		return SetPasscodeResult::Failed;
	} else if (!_keyData->passcodeWraps.empty()) {
		return SetPasscodeResult::NeedsVerification;
	} else if (_keyData->openKeyEncrypted.isEmpty()
		|| !_keyData->openKeyVerified) {
		LOG(("App Error: refusing a passcode without the app lock "
			"over an unverified open wrap."));
		return SetPasscodeResult::Failed;
	}
	return installPasscode(derived, true);
}

// Turning the lock off installs an open wrap over a file a passcode still
// protects - the same weakening of the data at rest a removal is - so it
// carries the same proof a removal does. Turning it on proves nothing new
// and only needs a passcode wrap to exist. The gsl::finally still spends the
// nonce on every exit, whichever way the lock moves, so a token that
// authorized one change cannot authorize another.
SetPasscodeResult Domain::setAppLockEnabled(
		bool enabled,
		PasscodeVerification verification) {
	if (IsFakeWithoutInfinityFlag()) {
		const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });
		if (enabled) {
			return SetPasscodeResult::Success;
		} else if (!accepts(verification)) {
			return SetPasscodeResult::NeedsVerification;
		}
		_verificationNonce = 0;
		setFakePasscode({});
		return SetPasscodeResult::Success;
	}
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	const auto hasWrap = !_keyData->passcodeWraps.empty();
	if (_keyData->legacy) {
		LOG(("App Error: refusing an app lock change before the migration."));
		return SetPasscodeResult::Failed;
	} else if ((enabled && !hasWrap)
		|| (!enabled && hasWrap && !accepts(verification))) {
		return SetPasscodeResult::NeedsVerification;
	}
	auto updated = *_keyData;
	if (enabled) {
		dropOpenWrap(updated);
	} else {
		installOpenWrap(updated);
	}
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

// Main::Domain::removePasscodeIfEmpty() calls this after Local::reset() has
// destroyed every store the local key protected. Nothing remains for the old
// passcode to guard or to ask for it - the account it belonged to is gone.
// Wallet's confirmed forgotten-passcode reset calls it after the shared
// keyring and all signed-in custody records are gone. DropUnusedPasscode()
// calls it when the verified app lock is off and no live keyring entry uses
// the Passcode policy, at startup, logout and terminal wallet operations.
// Each caller establishes that the passcode guards nothing left to ask
// for, which is why this removal carries no verification and has its own
// name instead of being reachable through setPasscode(). The write stays
// checked so failure does not claim the passcode was removed.
void Domain::clearPasscodeAfterReset() {
	if (IsFakeWithoutInfinityFlag()) {
		setFakePasscode({});
		return;
	}
	Expects(_localKey != nullptr);

	auto updated = *_keyData;
	updated.legacy = false;
	updated.legacyPasscode = false;
	updated.passcodeWraps.clear();
	updated.committed = _keyData->committed + 1;
	installOpenWrap(updated);

	Assert(!updated.openKeyEncrypted.isEmpty());
	if (!writeKeyDataChecked(updated)) {
		return;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;
	_verificationNonce = 0;

	_passcodeKeyChanged.fire({});
}

bool Domain::checkFakePasscode(const QByteArray &passcode, size_t fakeIndex) const {
    if (fakeIndex >= _fakePasscodes.size()) {
        return false;
    }
    const auto checkKey = CreateLocalKey(passcode, _passcodeKeySalt);
    return checkKey->equals(_fakePasscodes[fakeIndex].GetFakePasscodeKey());
}

void Domain::setPasscode(const QByteArray &passcode) {
	if (IsFake()) {
		PTG::SetHWLockEnabled(_keyData->fake.hardwareBound);
		return;
            }
	const auto nextHardware = PTG::IsHWLockEnabled();
	PTG::SetHWLockEnabled(_keyData->fake.hardwareBound);
	const auto verified = verifyPasscode(passcode);
	PTG::SetHWLockEnabled(nextHardware);
	if (!verified || setPasscode(passcode, *verified) != SetPasscodeResult::Success) {
		PTG::SetHWLockEnabled(_keyData->fake.hardwareBound);
		LOG(("App Error: could not rewrap the verified passcode."));
        }
}

int Domain::oldVersion() const {
	return _oldVersion;
}

void Domain::clearOldVersion() {
	_oldVersion = 0;
}

rpl::producer<> Domain::localPasscodeChanged() const {
	return _passcodeKeyChanged.events();
}

bool Domain::hasPasscode() const {
	return !_keyData->passcodeWraps.empty() || _keyData->legacyPasscode;
}

bool Domain::appLockEnabled() const {
	return _keyData->legacyPasscode || !_keyData->openKeyVerified;
}

bool Domain::hasLocalPasscode() const {
	// This historical API means verified app locking, not a wallet passcode.
	return appLockEnabled();
}

[[nodiscard]] Domain::StartModernResult Domain::tryFakeStart(
        const QByteArray& keyEncrypted,
        const QByteArray& infoEncrypted,
        const QByteArray& salt,
        const QByteArray &passcode) {
    _fakePasscodes.resize(_fakePasscodeKeysEncrypted.size());
    for (qint32 i = 0; i < qint32(_fakePasscodeKeysEncrypted.size()); ++i) {
        if (salt.size() != LocalEncryptSaltSize) {
            LOG(("App Error: bad salt in info file, size: %1").arg(salt.size()));
            return StartModernResult::IncorrectPasscode;
        }

        EncryptedDescriptor keyInnerData;
        if (!DecryptLocal(keyInnerData, _fakePasscodeKeysEncrypted[i], _passcodeKey)) {
            continue;
        }
        if (!_keyData->legacy) {
            _isStartedWithFake = true;
            _fakePasscodeIndex = i;
            const auto result = startUsingKeyStream(keyInnerData, keyEncrypted,
                infoEncrypted, salt, true);
            return result == StartModernResult::Success
                ? result
                : StartModernResult::IncorrectPasscode;
        }
        if (keyInnerData.data.size() < sizeof(MTP::AuthKey::Data)) {
            QByteArray fullPasscode;
            keyInnerData.stream >> fullPasscode;
            _passcodeKey = CreateLocalKey(fullPasscode, salt);
        } else {
            auto sourcePasscodeKey = Serialize::read<MTP::AuthKey::Data>(keyInnerData.stream);
            _passcodeKey = std::make_shared<MTP::AuthKey>(sourcePasscodeKey);
        }
        _isStartedWithFake = true;
        _fakePasscodeIndex = i;
        FAKE_LOG(qsl("Start with fake passcode %1").arg(i));
        break;
    }

    if (_isStartedWithFake) {
        // _passcodeKey already decrypted to local
        EncryptedDescriptor realKeyInnerData;
        if (!DecryptLocal(realKeyInnerData, keyEncrypted, _passcodeKey)) {
            return StartModernResult::IncorrectPasscode;
        }
        const auto result = startUsingKeyStream(realKeyInnerData, keyEncrypted, infoEncrypted, salt, true);
        return result == StartModernResult::Success ? result : StartModernResult::IncorrectPasscode;
    } else {
        if (PTG::SuppressHWLockLogErrors() != PTG::SuppressHWLockLogErrorsLevel::SUPPRESS_ERRORS_ONLY) {
            LOG(("App Info: could not decrypt pass-protected key from info file, "
                 "maybe bad password..."));
        }
        return StartModernResult::IncorrectPasscode;
    }
}

Domain::StartModernResult Domain::startUsingKeyStream(EncryptedDescriptor& keyInnerData,
                                                      const QByteArray& keyEncrypted,
                                                      const QByteArray& infoEncrypted,
                                                      const QByteArray& salt,
                                                      bool hasPasscode) {
    EncryptedDescriptor info;
    auto key = Serialize::read<MTP::AuthKey::Data>(keyInnerData.stream);
    if (keyInnerData.stream.status() != QDataStream::Ok
        || !keyInnerData.stream.atEnd()) {
        LOG(("App Error: could not read pass-protected key from info file"));
        return StartModernResult::Failed;
    }
    _localKey = std::make_shared<MTP::AuthKey>(key);

    _passcodeKeyEncrypted = keyEncrypted;
    _passcodeKeySalt = salt;
    _hasLocalPasscode = hasPasscode;

    if (!DecryptLocal(info, infoEncrypted, _localKey)) {
        LOG(("App Error: could not decrypt info."));
        return StartModernResult::IncorrectPasscode;
    }
    if (!_keyData->legacy && (_keyData->ptg || HasFakePasscodeCount(info.data))
        && !ValidateFakePasscodeCount(info.data, int(_fakePasscodes.size()))) {
        LOG(("App Error: PTG fake-passcode metadata does not match encrypted info."));
        return StartModernResult::IncorrectPasscode;
    }
    LOG(("App Info: reading encrypted info..."));
    auto count = qint32();
    info.stream >> count;
    if (count > Main::Domain::kAbsoluteMaxAccounts()) {
        LOG(("App Error: bad accounts count: %1").arg(count));
        return StartModernResult::Failed;
    }
    auto set_account = base::flat_set<qint32>();
    auto active = 0;
    auto loaded_accounts = std::vector<qint32>();

    const auto populateAccountIndex = [&] (qint32 index) {        
        if (!set_account.contains(index)) {
            set_account.emplace(index);
            loaded_accounts.push_back(index);
        }
    };

    for (auto i = 0; i != count; ++i) {
        auto index = qint32();
        info.stream >> index;
        populateAccountIndex(index);
    }

    if (!info.stream.atEnd()) {
        info.stream >> active;
    }

    if (!info.stream.atEnd()) {
        info.stream >> _isInfinityFakeModeActivated;
        FAKE_LOG(("StorageDomain: startUsingKey: Read serialized flag: " +
                   QString::number(_isInfinityFakeModeActivated)));

        if (!_isInfinityFakeModeActivated) {
            qint32 serialized_version;
            info.stream >> serialized_version;

            // Maybe some actions with migration
            FAKE_LOG(qsl("Read PTelegram version: %1").arg(serialized_version));

            for (auto& fakePasscode : _fakePasscodes) {
                QByteArray serializedActions, pass;
                QByteArray serializedName;
                info.stream >> serializedActions >> pass >> serializedName;
                QString name = QString::fromUtf8(serializedName);
                fakePasscode.SetPasscode(pass);
                fakePasscode.SetName(name);
                fakePasscode.DeSerializeActions(serializedActions);

                if (fakePasscode.ContainsAction(FakePasscode::ActionType::Logout)) {
                    auto* logout = dynamic_cast<FakePasscode::LogoutAction*>(
                            fakePasscode[FakePasscode::ActionType::Logout]
                        );
                    const auto& logout_accounts = logout->GetAccounts();
                    for (const auto& index : logout_accounts) {
                        populateAccountIndex(index);
                    }
                }
            }

            info.stream >> _isCacheCleanedUpOnLock;
            info.stream >> _isAdvancedLoggingEnabled;

            if (!info.stream.atEnd()) {
                info.stream >> _isErasingEnabled;
            }

            if (!_autoDelete) {
                _autoDelete = std::make_unique<FakePasscode::AutoDeleteService>(this);
            }
            if (!info.stream.atEnd()) {
                QByteArray autoDeleteData;
                info.stream >> autoDeleteData;
                _autoDelete->DeSerialize(autoDeleteData);
            }
            if (!info.stream.atEnd()) {
                info.stream >> _cacheFolderPermissionRequested;
            }
            // added 1.7.0
            if (!info.stream.atEnd()) {
                loaded_accounts.clear();
                auto actual_count = qint32();
                info.stream >> actual_count;
                FAKE_LOG(qsl("Found full list of %1 accounts").arg(actual_count));
                for (auto i = 0; i < actual_count; ++i) {
                    auto index = qint32();
                    info.stream >> index;
                    loaded_accounts.push_back(index);
                }
                FAKE_LOG(qsl("Loaded %1 accounts").arg(loaded_accounts.size()));
            }
            // added 1.8.4
            if (!info.stream.atEnd()) {
                PTG::DASettings::deserialize(info.stream);
            }
            // added PTG 2.0
            if (!info.stream.atEnd()) {
                PTG::deserialize(info.stream);
            }
        } else {
            if (_autoDelete) {
                _autoDelete->DeleteAll();
            }
        }
    }
    else
    {
        if (!_autoDelete) {
            _autoDelete = std::make_unique<FakePasscode::AutoDeleteService>(this);
        }
    }

    if (_isStartedWithFake) {
        auto matched = false;
        for (auto i = size_t(); i != _fakePasscodes.size(); ++i) {
            if (_fakeUnlockKey && _fakeUnlockKey->equals(_fakePasscodes[i].GetFakePasscodeKey())) {
                _fakePasscodeIndex = int(i);
                matched = true;
                break;
            }
        }
        if (!matched) {
            return StartModernResult::IncorrectPasscode;
        }
    }

    // Actually load accounts
    auto tried = base::flat_set<int>();
    auto sessions = base::flat_set<qint32>();
    qint32 realCount = 0;
    for (auto i = 0; i != loaded_accounts.size(); ++i) {
        int index = loaded_accounts[i];

        if (index >= 0
            && index < Main::Domain::kAbsoluteMaxAccounts()
            && tried.emplace(index).second) {
            FAKE_LOG(qsl("Add account %1 with seq_index %2").arg(index).arg(i));
            auto account = std::make_unique<Main::Account>(
                _owner,
                _dataName,
                index);
            auto config = account->prepareToStart(_localKey);
            const auto sessionId = account->willHaveSessionUniqueId(
                config.get());
            if (!sessions.contains(sessionId)
                && (sessionId != 0 || (sessions.empty() && (i + 1 == loaded_accounts.size())))) {
                account->start(std::move(config));
                if (index >= Main::Domain::kPremiumMaxAccounts())
                {
                    account->setHiddenMode(true);
                }
                _owner->accountAddedInStorage({
                    .index = index,
                    .account = std::move(account)
                    });
                sessions.emplace(sessionId);
            }
            ++realCount;
        }
    }
    count = realCount;

    // post init
    if (!_isInfinityFakeModeActivated) {
        for (auto& fakePasscode : _fakePasscodes) {
            fakePasscode.PostInit();
        }
    }

    FAKE_LOG(qsl("After all we have %1 accounts").arg(count));
    if (count <= 0) {
        LOG(("App Error: bad accounts count: %1").arg(count));
        return StartModernResult::Failed;
    }

    if (sessions.empty()) {
        LOG(("App Error: no accounts read."));
        return StartModernResult::Failed;
    }

    if (tried.find(active) == tried.end()) {
        // not found
        active = 0;
    }

    FAKE_LOG(("StorageDomain: startModern: Active: " + QString::number(active)));
    _owner->activateFromStorage(active);

    FAKE_LOG(("StorageDomain: startModern: Session empty?: " + QString::number(sessions.empty())));
    Ensures(!sessions.empty());

    return StartModernResult::Success;
}

const std::deque<FakePasscode::FakePasscode> &Domain::GetFakePasscodes() const {
    return _fakePasscodes;
}

void Domain::EncryptFakePasscodes() {
	_fakePasscodeKeysEncrypted = fakeKeyWraps(_keyData->legacy, _keyData->fake.hardwareBound);
}

std::vector<QByteArray> Domain::fakeKeyWraps(bool legacy, bool hardwareBound) const {
	auto result = std::vector<QByteArray>();
	if (_isInfinityFakeModeActivated) {
		return result;
	}
	const auto &key = legacy ? _passcodeKey : _localKey;
	Expects(key != nullptr);
	for (const auto &fake : _fakePasscodes) {
		EncryptedDescriptor inner(MTP::AuthKey::kSize);
		key->write(inner.stream);
		result.push_back(PrepareEncrypted(inner, CreateLocalKey(fake.GetPasscode(),
			_passcodeKeySalt, hardwareBound)));
	}
	return result;
}

size_t Domain::AddFakePasscode(QByteArray passcode, QString name) {
    FAKE_LOG(qsl("Add passcode with name %1").arg(name));
    FakePasscode::FakePasscode fakePasscode;
    fakePasscode.SetPasscode(std::move(passcode));
    fakePasscode.SetName(std::move(name));
    fakePasscode.PostInit();
    _fakePasscodes.push_back(std::move(fakePasscode));
    FAKE_LOG(qsl("Call write accounts from AddFakePasscode"));
    writeAccounts();
    _fakePasscodeChanged.fire({});
    return _fakePasscodes.size() - 1; // current index
}

void Domain::SetFakePasscode(QByteArray passcode, QString name, size_t fakeIndex) {
	_verificationNonce = 0;
    FAKE_LOG(("Setup passcode with name"));
    _fakePasscodes[fakeIndex].SetPasscode(std::move(passcode));
    _fakePasscodes[fakeIndex].SetName(std::move(name));
    FAKE_LOG(qsl("Call write accounts from SetFakePasscode"));
    writeAccounts();
    _fakePasscodeChanged.fire({});
}

rpl::producer<QString> Domain::GetFakePasscodeName(size_t fakeIndex) const {
    return _fakePasscodes[fakeIndex].GetName();
}

void Domain::SetFakePasscodeName(QString newName, size_t fakeIndex) {
    FAKE_LOG(("Setup passcode name"));
    _fakePasscodes[fakeIndex].SetName(std::move(newName));
}

void Domain::RemoveFakePasscode(size_t index) {
	_verificationNonce = 0;
    FAKE_LOG(qsl("Remove passcode %1").arg(index));
    _fakePasscodes.erase(_fakePasscodes.begin() + index);
    _fakePasscodeKeysEncrypted.erase(_fakePasscodeKeysEncrypted.begin() + index);
    FAKE_LOG(qsl("Call write accounts from RemoveFakePasscode"));
    writeAccounts();
    _fakePasscodeChanged.fire({});
}

rpl::producer<size_t> Domain::GetFakePasscodesSize() {
    return rpl::single(
            _fakePasscodes.size()
    ) | rpl::then(
            _fakePasscodeChanged.events() | rpl::map([=] { return _fakePasscodes.size(); }));
}

bool Domain::CheckFakePasscodeExists(const QByteArray& passcode) const {
    for (const auto& existed_passcode: _fakePasscodes) {
        if (existed_passcode.CheckPasscode(passcode)) {
            return true;
        }
    }

	return checkPasscode(passcode);
}

FakePasscode::Action* Domain::AddAction(size_t index, FakePasscode::ActionType type) {
    FAKE_LOG(qsl("Add action of type %1 to passcode %2").arg(static_cast<int>(type)).arg(index));
    _fakePasscodes[index].AddAction(FakePasscode::CreateAction(type));
    _fakePasscodeChanged.fire({});

    return _fakePasscodes[index][type];
}

void Domain::RemoveAction(size_t index, FakePasscode::ActionType type) {
    FAKE_LOG(qsl("Remove action of type %1 to passcode %2").arg(static_cast<int>(type)).arg(index));
    _fakePasscodes[index].RemoveAction(type);
    _fakePasscodeChanged.fire({});
}

bool Domain::ContainsAction(size_t index, FakePasscode::ActionType type) const {
    FAKE_LOG(qsl("Check action of type %1 of passcode %2").arg(static_cast<int>(type)).arg(index));
    return _fakePasscodes[index].ContainsAction(type);
}

void Domain::ExecuteIfFake() {
    if (IsFakeWithoutInfinityFlag()) {
        _owner->walletKeyring().clear();
        _owner->walletKeyring().setUnusable(true);
        FAKE_LOG(qsl("Execute fake passcode %1").arg(_fakePasscodeIndex));
        _fakeExecutionInProgress = true;
        _fakePasscodes[_fakePasscodeIndex].Execute();
        _fakeExecutionInProgress = false;
        FAKE_LOG(qsl("Call write accounts from ExecuteIfFake"));
        writeAccounts();
    }
}

bool Domain::CheckAndExecuteIfFake(const QByteArray &passcode) {
	return executeFakeKey(CreateLocalKey(passcode, _passcodeKeySalt));
}

bool Domain::checkAndExecutePasscode(PasscodeDerivation derived) {
	const auto fakeKey = derived._fakeKey;
	const auto correct = checkPasscode(std::move(derived));
	if (fakeKey && executeFakeKey(fakeKey)) {
		return true;
	} else if (correct) {
		SetFakePasscodeIndex(-1);
	}
	return correct;
}

bool Domain::executeFakeKey(const MTP::AuthKeyPtr &key) {
	for (auto i = size_t(); i != _fakePasscodes.size(); ++i) {
		if (!key->equals(_fakePasscodes[i].GetFakePasscodeKey())) {
			continue;
		} else if (i == _fakePasscodeIndex && !_isStartedWithFake) {
			return true;
		} else if (_fakePasscodeIndex != -1 && i != _fakePasscodeIndex) {
			continue;
		}
		_fakePasscodeIndex = i;
		ExecuteIfFake();
		_isStartedWithFake = false;
		return true;
	}
	return false;
}

void Domain::finishPasscodeUnlock() {
	if (IsFakeWithoutInfinityFlag() && _isStartedWithFake) {
		ExecuteIfFake();
		_isStartedWithFake = false;
	}
}

bool Domain::IsFake() const {
    return _fakePasscodeIndex >= 0 || _isInfinityFakeModeActivated;
}

bool Domain::IsFakeWithoutInfinityFlag() const {
    return _fakePasscodeIndex >= 0;
}

bool Domain::IsFakeInfinityFlag() const {
    return _isInfinityFakeModeActivated;
}

void Domain::SetFakePasscodeIndex(qint32 index) {
    _verificationNonce = 0;
    _fakePasscodeIndex = index;
    if (_owner->started()) {
        _owner->walletKeyring().clear();
        _owner->walletKeyring().setUnusable(IsFake());
    }
}

bool Domain::checkRealOrFakePasscode(const QByteArray &passcode) const {
    if (IsFakeWithoutInfinityFlag()) {
        return checkFakePasscode(passcode, _fakePasscodeIndex);
    } else {
        return checkPasscode(passcode);
    }
}

const FakePasscode::Action* Domain::GetAction(size_t index, FakePasscode::ActionType type) const {
    return _fakePasscodes[index][type];
}

QString Domain::GetCurrentFakePasscodeName(size_t fakeIndex) const {
    if (fakeIndex >= _fakePasscodes.size()) {
        return "";
    }
    return _fakePasscodes[fakeIndex].GetCurrentName();
}

FakePasscode::Action *Domain::AddOrGetIfExistsAction(size_t index, FakePasscode::ActionType type) {
    if (!ContainsAction(index, type)) {
        AddAction(index, type);
    }

    return _fakePasscodes[index][type];
}

FakePasscode::Action *Domain::GetAction(size_t index, FakePasscode::ActionType type) {
    return _fakePasscodes[index][type];
}

void Domain::PrepareEncryptedFakePasscodes() {
    for (size_t i = 0; i < _fakePasscodes.size(); ++i) {
        _fakePasscodes[i].ReEncryptPasscode();
    }
}

bool Domain::HasAccountForLogout(qint32 account_index) const {
    for (size_t i = 0; i < _fakePasscodes.size(); ++i) {
        if (auto *action = _fakePasscodes[i][FakePasscode::ActionType::Logout]) {
            FakePasscode::LogoutAction* logout = (FakePasscode::LogoutAction*)action;
            if (logout->GetData(account_index).Kind == FakePasscode::HideAccountKind::Logout) {
                return true;
            }
        }
    }
    return false;
}

bool Domain::IsCacheCleanedUpOnLock() const {
    return _isCacheCleanedUpOnLock;
}

void Domain::SetCacheCleanedUpOnLock(bool cleanedUp) {
    FAKE_LOG(("Setup cache cleaned up to %1").arg(cleanedUp));
    _isCacheCleanedUpOnLock = cleanedUp;
}

void Domain::ClearFakeState() {
    _fakePasscodes.clear();
    _fakePasscodeIndex = -1;
    _fakePasscodeKeysEncrypted.clear();
    _isCacheCleanedUpOnLock = false;
    _isAdvancedLoggingEnabled = false;
    _isErasingEnabled = false;
}

bool Domain::IsAdvancedLoggingEnabled() const {
    return _isAdvancedLoggingEnabled;
}

void Domain::SetAdvancedLoggingEnabled(bool loggingEnabled) {
    FAKE_LOG(("Setup advanced logging to %1").arg(loggingEnabled));
    _isAdvancedLoggingEnabled = loggingEnabled;
}

bool Domain::IsErasingEnabled() const {
    return _isErasingEnabled;
}

void Domain::SetErasingEnabled(bool enabled) {
    FAKE_LOG(("Setup DoD cleaning State to %1").arg(enabled));
    _isErasingEnabled = enabled;
}

[[nodiscard]] QByteArray Domain::GetPasscodeSalt() const {
	return _passcodeKeySalt;
}

qint32 Domain::GetFakePasscodeIndex() const{
    return _fakePasscodeIndex;
}

void Domain::ClearActions(size_t index) {
    _fakePasscodes[index].ClearActions();
}

void Domain::ClearCurrentPasscodeActions(){
    ClearActions(_fakePasscodeIndex);
}

FakePasscode::AutoDeleteService *Domain::GetAutoDelete() const {
    return _autoDelete.get();
}

void Domain::cacheFolderPermissionRequested(bool val) {
    _cacheFolderPermissionRequested = val;
    writeAccounts();
}

bool Domain::checkPasscode(const QByteArray &passcode) const {
	auto derived = prepareOpen(passcode);
	derived.run();
	return checkPasscode(std::move(derived));
}

} // namespace Storage
