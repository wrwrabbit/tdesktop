# PTG local storage and passcode compatibility

This guide covers the account domain and passcode storage in `tdata`. PTG 2.3.0 is the released baseline. PTG 2.4.0 refers to the storage adaptation with merge of Telegram Desktop 7.4.0. The layout and migration described here are based on the code. Upgrade and restart behavior on a real profile still need validation.

## Intent

Preserve existing PTG accounts, session data, fake passcodes, actions, and settings when upgrading. Reuse Telegram Desktop’s local storage and local key. A passcode change or upgrade should change how the local key is wrapped, while the account data continues to use that same key. A successful real unlock should preserve the app lock state. A fake unlock must select the configured fake identity and run the actions for that identity at the appropriate point in startup or unlock.

PTG adds data at two layers. The outer key file contains the salt and the encrypted key material needed before an unlock. The account payload inside that file is encrypted with the local key and holds the account indexes and PTG settings. The per-account stores continue to use the local key. The key file lives under `tdata` with a logical name of `key_<dataName>`, as defined by `ComputeKeyName()` in [storage_domain.cpp](../Telegram/SourceFiles/storage/storage_domain.cpp).

## Compatibility use cases

| Profile and reader | Intended behavior | What the current format supports |
| --- | --- | --- |
| Upstream `tdata` opened by PTG | Open with the existing local passcode, load the accounts and common Telegram settings, and initialize PTG state with its defaults. | The 2.4.0 reader accepts the three-field legacy layout and the modern `KD2` layout without `PTG3`. An absent PTG payload means there are no fake passcodes or PTG settings to restore. A subsequent PTG write can add those records. |
| PTG `tdata` opened by a compatible upstream build | Open cleanly with the real local passcode, load the accounts in the upstream-visible list, and load common settings understood by that upstream build. | The 7.4.0 reader can read a matching `KD2` trailer and leaves the following `PTG3` record unread. It reads the common account prefix and leaves the PTG suffix unread. The real passcode depends on a compatible wrap and KDF. A hardware-bound PTG passcode also depends on PTG’s platform-specific derivation and may be unusable in stock Telegram. |
| Older PTG `tdata` opened by newer PTG | Retain both account lists, fake identities and actions, PTG settings, and app-lock state. | The 2.4.0 reader recognizes the 2.3.0 layout. A real unlock can migrate the key wraps. A fake-first unlock keeps the legacy layout until a real unlock can migrate it. See [Direct upgrade and unlock](#direct-upgrade-and-unlock). |
| Older PTG `tdata` opened by newer upstream Telegram | Use the old real passcode and common data where possible, and minimize failures caused by PTG extensions. An unsupported profile should fail cleanly and remain recoverable. | A 2.3.0 file with a decimal fake-wrap count after the third field can be rejected by stock 7.4.0. Its reader treats that next field as a `KD2` trailer and expects the upstream magic. A legacy file that ends after the third field can follow the upstream legacy path. This direction is a compatibility goal with a known format conflict. |

The upstream-visible account list is the compatibility boundary. Stock Telegram should see the accounts PTG put in that list and ignore the separate full PTG list. Common settings are the fields and per-account stores that both builds understand. Stock Telegram does not restore PTG settings or run fake-passcode actions.

Opening a profile and preserving it on write are separate checks. An upstream writer can successfully open a modern PTG profile and later rewrite the key file without the PTG trailer or encrypted suffix. That can remove the fake-passcode configuration and the full account index list from the rewritten key file. Use separate copies of `tdata` when testing cross-client compatibility or switching back to PTG.

## Released PTG 2.3.0

The outer file starts with the upstream three-field sequence. Each field is a serialized byte array.

```text
Outer key-file stream
  [salt] [encrypted local key] [encrypted account info] [decimal count] [fake wraps...]
  |--------------- upstream prefix -----------------| |--- legacy PTG trailer ----|
```

The legacy PTG trailer is a continuation of the outer stream after the encrypted account-info blob. Its first field is the fake-passcode count as a decimal `QByteArray`, followed by that many encrypted fake wraps. This legacy trailer has no `PTG3` magic or separate enclosing record. `PTG3` is the modern PTG trailer that follows `KD2`. A stock legacy Telegram profile ends after the third field. The 2.3.0 infinity-mode writer also omits the fake-wrap suffix.

The real passcode derives a key that opens the encrypted local key. Each legacy fake wrap opens to the **real passcode-derived key**, which then opens the local key. The legacy reader also has a compatibility path for older fake envelopes containing passcode bytes. This is why upgrading the real-passcode key format affects fake unlock even when the user keeps the same fake passcodes.

Inside the encrypted account info, PTG retains the upstream prefix of account count, account indexes, and active account index. The prefix omits indexes selected by PTG logout or hide actions. PTG then appends its infinity-mode flag. In regular mode the suffix includes the PTG version, each fake passcode’s serialized actions, passcode bytes and name, the cleanup, logging and erasing flags, auto-delete state, cache-folder permission, the full account index list, and the `PTG::DASettings` and `PTG` settings blocks. The passcode bytes and actions are inside the encrypted payload. The full account list lets PTG recover the accounts omitted from the upstream prefix. Preserve both lists and their distinct roles when changing the writer. [PTG settings serialization](../Telegram/SourceFiles/fakepasscode/settings.cpp)

## PTG 2.4.0 on Telegram Desktop 7.4.0

`ReadKeyData()` distinguishes the legacy three-field file, the legacy PTG decimal count and fake wraps, and the modern upstream key-wrap trailer. The modern PTG file has this layout.

```text
open salt | open local-key wrap | encrypted account info
| upstream KD2 key-wrap trailer | PTG3 fake-passcode trailer
```

The upstream `KD2` trailer records the committed generation and the passcode wraps. Each wrap carries its key derivation family, cost parameters, salt, generation, and encrypted local key. The modern real passcode uses the upstream memory-hard derivation. An open wrap can coexist with a passcode wrap when the app lock is off and a passcode still protects another feature. When the app lock is on, the open-wrap field is empty and the salt retains the expected size. The app lock and the presence of a passcode are therefore separate pieces of state.

The separate, length-delimited `PTG3` trailer holds its format version, fake salt, fake-wrap count, encrypted wraps, and hardware-binding flag. The reader checks the format, lengths and count before using the entries. Each modern fake wrap opens directly to the **local key**. A real-passcode change can replace the real wrap while continuing to rebuild the fake wraps from the local key. [PTG trailer codec](../Telegram/SourceFiles/storage/details/storage_fake_passcode_data.cpp)

The two PTG records have different jobs. `PTG3` contains only the information needed to try an unlock before the local key is available. The encrypted account info contains the application state, including the PTG settings that 2.3.0 already stored there.

| Location in 2.4.0 | Data |
| --- | --- |
| Outer `PTG3` trailer | Format version, fake salt, fake-wrap count, encrypted wraps of the local key, and the hardware-binding flag used for derivation. |
| Encrypted account info, common prefix | Upstream-visible account count and indexes, followed by the active account index where present. |
| Encrypted account info, PTG suffix | Infinity-mode flag and, in regular mode, PTG version, fake-passcode names and passcode bytes, action definitions, cleanup/logging/erasing flags, auto-delete data, cache-folder permission, the full account index list, and `PTG::DASettings` and `PTG` settings. |
| Encrypted account info, `PTGC` footer | Fake-passcode count protected by the account payload encryption. |

`DASettings` and the other PTG settings stay in encrypted account info in 2.4.0. The hardware-binding flag and fake wraps are the pieces the reader needs from `PTG3` before decryption. The count appears in both places so the encrypted payload can validate the count read from the outer trailer.

On modern PTG reads, a fake unlock matches the successful fake key against the passcodes in the decrypted payload before selecting an action set. The outer wrap’s position alone cannot select an action set. An infinity-mode write records zero fake wraps and omits the regular PTG payload. Preserve that behavior when adding fields.

## Direct upgrade and unlock

- **Existing 2.3.0 profile, real passcode.** The legacy reader opens the local key and decrypts the account payload first. `migrateFromLegacy()` creates a modern passcode wrap around that same local key and keeps the app lock on. A legacy profile opened without a passcode gets an open wrap. The successful startup queues a rewrite in the modern format. If deriving or verifying the new wrap fails, the state remains legacy and a later real unlock can retry migration.
- **Existing 2.3.0 profile, fake passcode first.** The legacy fake-wrap path can open the data using the legacy credentials. It keeps the legacy key layout because the real passcode required for the new real wrap is unavailable. A later startup with the real passcode can perform the migration. Hardware-bound legacy profiles can retry derivation with hardware protection on a supported device. The modern trailer records that choice for subsequent opens.
- **Already migrated profile.** The reader uses the committed passcode wrap or the open wrap, as appropriate. If a supplied passcode fails to open the real wrap, it can try the fake wraps. The same local key opens the account payload in either case. Telegram Desktop’s async `Main::Domain::tryPasscode()` path runs fake handling for an already running session and completes deferred fake actions after a successful cold start. [Async unlock](../Telegram/SourceFiles/main/main_domain.cpp)
- **Changing the real passcode after migration.** `installPasscode()` stages a new generation alongside the old one, reads it back and proves it opens the local key, then commits the new generation. Startup discards wraps outside the committed generation. Preserve the old credential through a failed or interrupted change and preserve the open-wrap state unless the operation explicitly changes the app lock.

## Adding fields and preserving compatibility

PTG keeps its extensions in the shared key file to preserve the appearance of a stock Telegram profile. This choice relies on keeping pace with upstream storage changes. Any upstream addition to the outer key-file stream or the encrypted account-info stream requires a PTG compatibility review and, where the layouts overlap, updates to both readers and writers and a migration for existing profiles. In particular, the position after `KD2` is shared. `PTG3` identifies the PTG record, but a future upstream reader may expect a different record there. Check both PTG-to-upstream and upstream-to-PTG opens before adopting a release that changes either stream.

- A newer reader must load all previously released layouts. Keep the upstream prefixes and field order, and read optional old suffix fields only when the applicable format contains them. Missing optional values need defaults that preserve old behavior. A first successful read of an old profile may rewrite it with the new defaults and the current format.
- `QDataStream` here is positional. Inserting a field into an existing sequence can make old and new readers interpret the remaining bytes differently. Extend the PTG payload with a defined version or a bounded sub-block and keep its count footer at the end. When reading optional fields, bound the payload so footer bytes cannot be consumed as settings. The current `PTG3` decoder requires a known format and consumes the full trailer. Adding bytes to it needs a reader change and an explicit format decision.
- Keep PTG fields outside the upstream key-wrap trailer. If upstream adds data after the account prefix or adds another outer record, review both readers and writers together. Today the PTG reader interprets bytes after the upstream account prefix as its own suffix, so future upstream extensions need an explicit discriminator or migration. Prefer the existing KV preferences facility for independent flags and values that do not need to live in the encrypted account payload.
- Treat malformed, truncated or unsupported key metadata as an unlock failure. Keep the file intact so the user can recover with a compatible reader. The current reader maps key trailer parse and unsupported KDF failures to an incorrect-passcode result. The outer file reader can also skip unreadable files and proceed to fallback paths. Verify file preservation explicitly when adding a new format or error case. [Outer file reader](../Telegram/SourceFiles/storage/details/storage_file_utilities.cpp)
- The file header version, upstream trailer format version and PTG version stored in the encrypted suffix serve different purposes. PTG currently writes `FakeAppVersion` in the outer file header. A header check alone cannot establish that an older binary understands newer wraps or PTG fields. After a newer binary rewrites a profile, an older writer may reject it, ignore additional fields, or drop them on save. Use a copy of the complete pre-upgrade `tdata` for rollback.

## Release validation

Validate a direct upgrade from a copy of released 2.3.0 data. Cover profiles with and without a real passcode, several fake identities with distinct actions, hidden accounts, and hardware binding where available. Exercise real-first and fake-first startup, warm unlock, wrong passcodes, restart after migration, and real-passcode change followed by fake unlock. Confirm the account lists, session data, actions, and settings survive the rewrite. Test an interrupted passcode change and invalid or unsupported metadata on disposable copies, checking that the last usable key file remains recoverable. Validate the signed updater separately by confirming the upgraded executable opens the same profile on restart.

Test each compatibility direction on its own copy. For upstream-to-PTG, verify that absent PTG records initialize cleanly. For PTG-to-upstream, test the real passcode, visible accounts and common settings on both the first open and restart. Check separately what survives an upstream rewrite. Include a 2.3.0 profile with fake wraps when testing newer upstream so the legacy trailer conflict is visible.
