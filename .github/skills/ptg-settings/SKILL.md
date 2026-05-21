---
name: ptg-settings
description: How PTG persistent settings work and how to add a new setting with full binary compatibility. Use when adding new PTG-specific settings that need to persist across sessions.
---

## Overview

PTG is designed so the two binaries can be swapped freely over a shared `tdata/` directory — PTG binary over TG data or TG binary over PTG data — without data loss or corruption. This contract must be preserved for every new PTG setting.

---

## Storage Locations

| Location | Owner | Rule |
|---|---|---|
| `tdata/settings` (`Core::Settings::serialize`) | Telegram | **PTG must never write here** |
| `tdata/key_<dataName>` encrypted info block | PTG | All PTG-specific settings go here |

### Why `tdata/settings` is off-limits

`Core::Settings::serialize()` writes a raw positional `QDataStream` blob with no block IDs. Fields are appended at the end with `!stream.atEnd()` guards. If PTG appends a field and upstream TG later adds its own field at the same offset, TG silently reads the PTG value into the wrong TG variable — corrupt settings, no error.

### The encrypted info block (`tdata/key_<dataName>`)

Written by `storage_domain.cpp::Domain::writeAccounts()`, read by `Domain::startUsingKeyStream()`. This is the canonical home for all PTG settings.

Current layout of the encrypted block (append-only, newest fields at the bottom):

```
qint32  account_indexes.size()          ← TG: visible account IDs
qint32  account_indexes[]
qint32  activeForStorage                ← TG: active account index
bool    _isInfinityFakeModeActivated    ← PTG  (guard: !stream.atEnd())
if !_isInfinityFakeModeActivated:
  qint32  PTelegramAppVersion           ← PTG version tag (= 2 000 000)
  for each fake passcode:
    QByteArray serializedActions
    QByteArray fakePasscode (encrypted)
    QByteArray fakeNames (UTF-8)
  bool    _isCacheCleanedUpOnLock
  bool    _isAdvancedLoggingEnabled
  bool    _isErasingEnabled             ← guard: !stream.atEnd()
  QByteArray autoDeleteData             ← guard: !stream.atEnd()
  bool    _cacheFolderPermissionRequested  ← guard: !stream.atEnd()
  // Added PTG 1.7.0
  qint32  full_account_indexes.size()   ← guard: !stream.atEnd()
  qint32  full_account_indexes[]
  // Added PTG 1.8.4
  PTG::DASettings::serialize()          ← guard: !stream.atEnd()
  //   → 5 × bool (dangerous-action checks)
  // Added PTG x.y.z
  PTG::serialize()                      ← guard: !stream.atEnd() (new general settings block)
```

**Forward compatibility rule:** every PTG field appended to the encrypted info block **must** be guarded by `!info.stream.atEnd()` on the read path. This lets PTG read tdata from any prior PTG version or from original TG without crashing.

---

## How to Add a New PTG Setting

### Step 1 — Declare the value

Add a static variable and accessor functions in the `PTG` namespace inside `fakepasscode/settings.cpp`:

```cpp
// fakepasscode/settings.cpp — inside namespace PTG
static TimeId vPrivacyLastReviewTime = 0;

TimeId GetPrivacyLastReviewTime() { return vPrivacyLastReviewTime; }
void SetPrivacyLastReviewTime(TimeId t) { vPrivacyLastReviewTime = t; }
```

Expose them in `fakepasscode/settings.h` inside `namespace PTG { ... }`.

### Step 2 — Add to the PTG serialize / deserialize pair

Do **not** add non-DA settings to `PTG::DASettings::serialize/deserialize`. `DASettings` is scoped to the five dangerous-action booleans.

Instead, use (or create) the separate top-level `PTG::serialize()` / `PTG::deserialize()` pair in `fakepasscode/settings.cpp`.

**Declare in `fakepasscode/settings.h`:**

```cpp
namespace PTG {
    void serialize(QDataStream& out);
    void deserialize(QDataStream& in);
}
```

**Implement in `fakepasscode/settings.cpp`** — append new fields at the end only:

```cpp
void serialize(QDataStream& out) {
    out << qint64(vPrivacyLastReviewTime)  // added PTG x.y.z
        << qint64(vLastSessionCheckTime);  // added PTG x.y.z
    // append new fields at the end; matching guard required in deserialize
}

void deserialize(QDataStream& in) {
    if (!in.atEnd()) { qint64 t; in >> t; vPrivacyLastReviewTime = TimeId(t); }
    if (!in.atEnd()) { qint64 t; in >> t; vLastSessionCheckTime  = TimeId(t); }
}
```

**Wire into `storage_domain.cpp`:**

In `writeAccounts()`, call it after `DASettings::serialize`:
```cpp
// Added 1.8.4
PTG::DASettings::serialize(keyData.stream);
// Added PTG x.y.z
PTG::serialize(keyData.stream);
```

In `startUsingKeyStream()`, read it after `DASettings::deserialize`:
```cpp
// added 1.8.4
if (!info.stream.atEnd()) {
    PTG::DASettings::deserialize(info.stream);
}
// added PTG x.y.z
if (!info.stream.atEnd()) {
    PTG::deserialize(info.stream);
}
```

### Step 3 — Access from UI/feature code

```cpp
#include "fakepasscode/settings.h"

// read
TimeId lastReview = PTG::GetPrivacyLastReviewTime();

// write — then trigger a domain write so writeAccounts() is called
PTG::SetPrivacyLastReviewTime(base::unixtime::now());
```

Trigger a domain write via the same call site used by `DASettings` setters (investigate the existing save trigger for the correct function).

---

## What Must NOT Happen

| Anti-pattern | Why it breaks compat |
|---|---|
| Add PTG field to `Core::Settings::serialize()` / `addFromSerialized()` | Shared with TG. Upstream TG adds its own field at the same offset → silent value corruption. |
| Insert PTG fields in the middle of the encrypted info block | Shifts stream positions for tdata written by older PTG — breaks deserialization. |
| Write a new top-level TDF file keyed by a PTG name | Only acceptable for a completely independent feature; stale values from previous installs are complex to reason about. |
| Omit `!stream.atEnd()` guard on the read path | Crashes when reading tdata from an older PTG or TG that did not write the field. |

---

## Data Map

| PTG data | Location | Mechanism |
|---|---|---|
| Fake passcode definitions | encrypted info block | `writeAccounts()` / `startUsingKeyStream()` |
| Fake passcode key blobs | outer plaintext | `writeAccounts()` |
| Hidden account indices | inside each fake's `LogoutAction` | `FakePasscode::SerializeActions()` |
| Dangerous-action flags | encrypted info block | `PTG::DASettings::serialize/deserialize` |
| Auto-delete config | encrypted info block | `AutoDeleteService::Serialize/DeSerialize` |
| **Future DA settings** | encrypted info block | Extend `PTG::DASettings::serialize/deserialize` |
| **Future general PTG settings** | encrypted info block | Extend `PTG::serialize/deserialize` |
| Core TG settings | `tdata/settings` | `Core::Settings::serialize()` — **PTG must not touch** |

---

## Quick Checklist

- [ ] Static variable + getter/setter declared in `fakepasscode/settings.cpp` and `settings.h`
- [ ] New field appended at the **end** of `PTG::serialize()` with a version comment
- [ ] Matching `if (!in.atEnd())` guard added at the **end** of `PTG::deserialize()`
- [ ] `PTG::serialize()` called in `Domain::writeAccounts()` after `DASettings::serialize`
- [ ] `PTG::deserialize()` called in `Domain::startUsingKeyStream()` after `DASettings::deserialize`
- [ ] Save trigger invoked after every setter call so `writeAccounts()` runs
- [ ] Field NOT added to `Core::Settings::serialize()` or `DASettings`
