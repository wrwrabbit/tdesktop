---
name: build-telegram
description: Trigger and monitor a Telegram Desktop Debug build without hitting terminal timeouts or VS Code confirmation dialogs.
---

## Overview

The Telegram Desktop Debug build takes **5–25 minutes** depending on how many files changed.
It has two distinct phases:

1. **Compilation phase** — many `.cpp` files compiled in parallel; `build_latest.txt` grows
   rapidly with one filename per line.
2. **Linking phase** — single-threaded, high memory, produces no output for **30–180 seconds**.
   This is normal. The log will appear to be frozen at the last compiled filename. Do **not**
   treat this silence as a hang or a failure.

`build.ps1` lives in this skill directory (`.github/skills/build-telegram/build.ps1`). It uses
`git rev-parse --show-toplevel` to locate the repo root regardless of where it is invoked from,
writes all cmake + MSBuild output to `<repoRoot>/build_latest.txt`, and appends
`BUILD_EXIT_CODE=<N>` as the very last line when done.

---

## Step 1 — Delete any stale output file

Always remove the previous `build_latest.txt` **before** starting a new build. Without this, an
early poll on a slow build can read the old file and give a false result.

```powershell
# isBackground=false, short command — safe
Remove-Item "d:\work\tdesktop\tdesktop2\build_latest.txt" -ErrorAction SilentlyContinue
```

---

## Step 2 — Start the build in the background

Use `run_in_terminal` with **`isBackground=true`**. The call returns immediately with a terminal
ID. Use the **absolute path** to the script and always pass `-ExecutionPolicy Bypass` — omitting
it silently fails on restricted-policy shells:

```powershell
# isBackground=true
powershell -ExecutionPolicy Bypass -File "d:\work\tdesktop\tdesktop2\.github\skills\build-telegram\build.ps1"
```

Do **not** use `.\build.ps1` (depends on cwd being the skill directory) or
`& "path\build.ps1"` (ignores execution policy).

---

## Step 3 — Poll for completion

Do **NOT** use `await_terminal` with a long timeout — VS Code will pop up a user confirmation
dialog for the agent. Do **NOT** chain `Start-Sleep` inside a non-background terminal
(`isBackground=false`) — the output is silently lost in buffering and the call appears to return
empty.

Instead, run a **short** `isBackground=false` command every minute or two:

```powershell
# isBackground=false — runs in ~1 second
Select-String -Path "d:\work\tdesktop\tdesktop2\build_latest.txt" -Pattern "^BUILD_EXIT_CODE" -ErrorAction SilentlyContinue
```

- Line **absent**: build is still running — wait and check again.
- `BUILD_EXIT_CODE=0`: build succeeded.
- Non-zero: build failed — go to Step 4.

You can also use `get_terminal_output` on the background terminal ID to see live compiler
progress without blocking.

### Polling cadence and the linking phase

Poll every **1 minute**. After compilation finishes (the last `.cpp` filename appears in the
log), do **not** panic if there is no new output for several minutes — this is the linker
working. Keep polling. Only consider the build stuck if **more than 5 minutes** pass with no
`BUILD_EXIT_CODE` line and the `.exe` LastWriteTime has not changed. Even then, before
declaring it stuck, ask the user to glance at VS 2022's output window directly, since some
errors (e.g. compiler heap exhaustion — see below) are not reliably written to the log file.

---

## Step 4 — Read errors on failure

```powershell
# isBackground=false
Get-Content "d:\work\tdesktop\tdesktop2\build_latest.txt" |
  Select-String -Pattern "error C|fatal error|error LNK"
```

---

## Heap exhaustion / PCH errors (not always in the log)

MSBuild can fail with **"not enough heap to create precompiled header"** or similar out-of-memory
errors. These may appear only in Visual Studio 2022's own Output pane and **not** in
`build_latest.txt`. If the build exits with a non-zero code but no clear error lines appear in
the log, tell the user:

> "Build failed but no error lines are visible in the log. This sometimes means a compiler
> heap/PCH error that only appears in VS 2022's Output window. Please check there. If
> everything looks clean, a reboot usually fixes it — memory tends to get saturated after a
> long session."

Do **not** attempt to diagnose or retry such a failure on your own.

---

## Locked-file rule

If the build fails with any of these errors:
- `fatal error C1041: cannot open program database`
- `cannot open output file 'Telegram.exe'`
- `LNK1104: cannot open file`
- Any "access denied" or "file in use" error

**Stop immediately. Do NOT retry.** Inform the user that files are locked and ask them to close
`Telegram.exe` (and any debugger). Wait for confirmation before rebuilding.

---

## Output locations

| Path | Description |
|---|---|
| `<repoRoot>/build_latest.txt` | Full cmake + MSBuild output; last line is `BUILD_EXIT_CODE=N` |
| `<repoRoot>/out/Debug/Telegram.exe` | Build artifact; check `LastWriteTime` to confirm freshness |

---

## Quick-reference

| Do | Don't |
|---|---|
| Delete `build_latest.txt` before building | Read stale output from a previous build |
| `run_in_terminal isBackground=true` + absolute path + `-ExecutionPolicy Bypass` | Use `.\build.ps1` (cwd-dependent) |
| Poll every ~2 min with short `Select-String` calls | Use `await_terminal` with timeout > ~60 s |
| Expect 30–180 s of silence during linking — this is normal | Panic and declare the build stuck during the linking phase |
| Use `get_terminal_output` to peek at live progress | Chain `Start-Sleep` in a non-background terminal |
| Ask user to check VS 2022 Output if log shows no errors but exit code ≠ 0 | Try to diagnose heap/PCH errors from the log alone |
| Always build **Debug** | Build Release (extremely heavy) |
