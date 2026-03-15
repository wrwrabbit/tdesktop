---
name: ptg-translation
description: How PTG (PTelegram) translation works and how to add translations for newly created strings.
---

## Overview

PTelegram adds its own UI strings on top of Telegram Desktop's native string system. Native Telegram strings are fetched from Telegram's servers per language; PTG strings are never on those servers so they need a separate in-code translation mechanism.

Two files are involved:

| File | Purpose |
|------|---------|
| `Telegram/Resources/langs/lang.strings` | English source of truth for **all** strings (native + PTG). PTG strings live at the bottom of this file, before `// Keys finished`. |
| `Telegram/SourceFiles/fakepasscode/lang/fakepasscode_translator.cpp` | Provides Russian (`ru`/`ru-*`), Belarusian (`be`/`be-*`), and Polish (`pl`/`pl-*`) translations for PTG strings. |

---

## Which strings need a translation in fakepasscode_translator.cpp?

**Only PTG-created strings** — those that do **not** exist in the upstream Telegram Desktop repository and were added by the PTG team.

They live in the PTG section near the bottom of `lang.strings` (roughly from `"lng_fakepasscode"` onward, until `// Keys finished`):

```
"lng_fakepasscode" / "lng_fakepasscodes_list" / "lng_fakeglobalaction_list" / "lng_fakeaccountaction_list" ...
"lng_show_fakes" / "lng_clear_proxy" / "lng_clear_cache" / "lng_clear_cache_on_lock" ...
"lng_logout" / "lng_hide" / "lng_special_actions" / "lng_command" ...
"lng_delete_contacts" / "lng_unblock_users" / "lng_delete_actions" ...
"lng_remove_chats" / "lng_chats_action_*" / "lng_autodelete_*" / "lng_send_autodelete_message" ...
"lng_open_spoof_*" / "lng_cant_change_value_title" / "lng_unhidden_limit_msg" ...
"lng_da_*" / "lng_dangerous_actions_help" / "lng_allow_dangerous_action*" ...
"lng_hw_lock_*" / "lng_version_mistmatch_*" / "lng_passcode_exists" ...
"lng_macos_cache_folder_permission_desc" ...
```

**Do NOT add** purely native Telegram strings (e.g., `"lng_settings_title"`, `"lng_box_ok"`, etc.) — they are already translated by Telegram's own language pack system.

> **Exception:** A small number of native keys (`lng_cancel`, `lng_continue`, `lng_profile_delete_my_messages`) are intentionally overridden in the translator because PTG needs slightly different wording. Only touch those if the PTG wording genuinely differs from the stock translation.

---

## How translations are injected

`GetExtraLangRecords(QString id)` in `fakepasscode_translator.cpp` returns a `const LangRecord*` array for the current language ID. The caller merges these records on top of the standard language pack, so PTG strings get translated and any overrides take effect.

Language matching: exact (`"ru"`, `"be"`, `"pl"`) then prefix (`startsWith("ru")`, etc.).  
Currently supported: **Russian**, **Belarusian**, **Polish**. Everything else falls through to English.

---

## How to add a new PTG string

### 1. Add the English string to lang.strings

Insert it in the PTG block, before `// Keys finished`:

```
"lng_my_new_string" = "English text here";
```

If the string uses placeholders (like `{caption}`) or plural forms (`#one` / `#other`), follow the same pattern as existing strings.

### 2. Add translations to fakepasscode_translator.cpp

Add the key/value pair to **every** translation array (`LangRuTranslation`, `LangByTranslation`, `LangPlTranslation`). The sentinel `{0, nullptr}` must always remain the last element — insert before it.

```cpp
// Russian
{"lng_my_new_string", "Русский текст"},

// Belarusian
{"lng_my_new_string", "Беларускі тэкст"},

// Polish
{"lng_my_new_string", "Polski tekst"},
```

### 3. Verify the static_assert

Each array ends with:

```cpp
static_assert(LangRuTranslation[sizeof(LangRuTranslation) / sizeof(LangRecord) - 1].key == 0);
```

This fires at compile time if the sentinel was accidentally removed. Additionally:

```cpp
static_assert(sizeof(LangRuTranslation) == sizeof(LangByTranslation));
```

This ensures `LangRuTranslation` and `LangByTranslation` have the same number of entries. **Keep all three arrays in sync** — same keys, same order. Missing a key in one array will trip this assert or produce untranslated strings.

---

## Mandatory rule for agents

> **Whenever you add a new string to `lang.strings` in the PTG block, you MUST immediately add matching translations to all three arrays in `fakepasscode_translator.cpp` (`LangRuTranslation`, `LangByTranslation`, `LangPlTranslation`). Never leave a PTG string without translations.**

If you cannot produce a quality translation for a language, use the English text as a placeholder and add a `// TODO: translate` comment directly after the entry — but do not skip the entry entirely.

---

## Quick checklist

- [ ] New key added to `lang.strings` in the PTG block (before `// Keys finished`)
- [ ] Russian translation added to `LangRuTranslation` (before `{0, nullptr}`)
- [ ] Belarusian translation added to `LangByTranslation` (before `{0, nullptr}`)
- [ ] Polish translation added to `LangPlTranslation` (before `{0, nullptr}`)
- [ ] All three arrays have equal size (`static_assert(sizeof(LangRuTranslation) == sizeof(LangByTranslation))` passes)
- [ ] No hardcoded English text used in C++ code — always reference via `tr::lng_my_new_string(tr::now)` or the reactive form
