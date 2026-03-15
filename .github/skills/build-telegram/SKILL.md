---
name: build-telegram
description: Trigger and monitor a Telegram Desktop Debug build without hitting terminal timeouts or VS Code confirmation dialogs.
---

## Overview

The Telegram Desktop Debug build takes **5–25 minutes** depending on how many files changed.

Use `build.ps1` (in this skill directory) to run the build. It writes all output to `build_latest.txt` at the repository root and appends `BUILD_EXIT_CODE=<N>` as the very last line when done.

## Step 1 — Copy build.ps1 to repo root and start the build

Run `build.ps1` with `isBackground=true` so the terminal call returns immediately:

```powershell
.\build.ps1
```

> Always invoke from the repository root so `build_latest.txt` is written there.

## Step 2 — Poll for completion

Do **NOT** use `await_terminal` with a long timeout — VS Code will show a user confirmation dialog.

Poll by checking whether the sentinel line has appeared:

```powershell
Select-String -Path ".\build_latest.txt" -Pattern "^BUILD_EXIT_CODE"
```

- Line **absent**: build is still running — wait a minute and check again.
- `BUILD_EXIT_CODE=0`: build succeeded.
- Non-zero: build failed — go to Step 3.

Alternatively, use `get_terminal_output` (not `await_terminal`) to peek at live progress without blocking.

## Step 3 — Read errors on failure

```powershell
Get-Content ".\build_latest.txt" |
  Select-String -Pattern "error C|fatal error|error LNK"
```

## Locked-file rule

If the build fails with any of these errors:
- `fatal error C1041: cannot open program database`
- `cannot open output file 'Telegram.exe'`
- `LNK1104: cannot open file`
- Any "access denied" or "file in use" error

**Stop immediately. Do NOT retry.** Inform the user that files are locked and ask them to close `Telegram.exe` (and any debugger). Wait for confirmation before rebuilding.

## Output locations

| Path | Description |
|---|---|
| `build_latest.txt` | Full cmake + MSBuild output; last line is `BUILD_EXIT_CODE=N` |
| `out/Debug/Telegram.exe` | Build artifact |

## Quick-reference

| Do | Don't |
|---|---|
| `run_in_terminal isBackground=true` → `.\build.ps1` | Run cmake directly |
| Poll `build_latest.txt` for `BUILD_EXIT_CODE=` | Use `await_terminal` with timeout > ~60 s |
| Use `get_terminal_output` to peek at progress | Use `run_in_terminal isBackground=false` for cmake |
| Always build **Debug** | Build Release (extremely heavy) |
