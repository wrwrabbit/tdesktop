---
name: task-think
description: Orchestrate a multi-phase implementation workflow for this repository with artifact files under .ai/<project-name>/<letter>/ using Codex subagents instead of shell-spawned child processes. Use when the user wants one prompt to drive context gathering, planning, plan assessment, implementation, build verification, and review with persistent artifacts and clear phase handoffs. Prefer spawn_agent/send_input/wait_agent; fall back to same-session execution only when delegation is unavailable or disallowed.
---

# Task Pipeline

Run a full implementation workflow with repository artifacts and clear phase boundaries.

## Inputs

Collect:
- task description
- optional project name (if missing, derive a short kebab-case name)
- optional constraints (files, architecture, risk tolerance)
- optional screenshot paths

If screenshots are attached in UI but not present as files, write a brief textual summary into the task artifacts before spawning fresh subagents so later phases can read the requirements without inheriting the whole parent thread.

## Overview

The workflow is organized around projects. Each project lives in `.ai/<project-name>/` and can contain multiple sequential tasks (labeled `a`, `b`, `c`, ... `z`).

Project structure:
```text
.ai/<project-name>/
  about.md              # Single source of truth for the entire project
  a/                    # First task
    context.md          # Gathered codebase context for this task
    plan.md             # Implementation plan
    review1.md          # Code review documents (up to 3 iterations)
    review2.md
    review3.md
    logs/
      phase-*.prompt.md
      phase-*.result.md
  b/                    # Follow-up task
    context.md
    plan.md
    review1.md
    logs/
      ...
  c/                    # Another follow-up task
    ...
```

- `about.md` is the project-level blueprint: a single comprehensive document describing what this project does and how it works, written as if everything is already fully implemented. It contains no temporal state ("current state", "pending changes", "not yet implemented"). It is rewritten, not appended to, each time a new task starts, incorporating the new task's changes as if they were always part of the design.
- Each task folder (`a/`, `b/`, ...) contains self-contained files for that task. The task's `context.md` carries all task-specific information: what specifically needs to change, the delta from the current codebase, gathered file references, and code patterns. Planning, implementation, and review phases should rely on the current task folder.

## Artifacts

Create and maintain:
- `.ai/<project-name>/about.md`
- `.ai/<project-name>/<letter>/context.md`
- `.ai/<project-name>/<letter>/plan.md`
- `.ai/<project-name>/<letter>/review<R>.md` (up to 3 review iterations)
- `.ai/<project-name>/<letter>/logs/phase-<name>.prompt.md`
- `.ai/<project-name>/<letter>/logs/phase-<name>.result.md`

Each `phase-<name>.result.md` should capture a concise outcome summary: whether the phase completed, which files it touched, and any follow-up notes or blockers.

## Phases

Run these phases sequentially:

1. Phase 0: Setup - Record start time, detect follow-up vs new project, create directories.
2. Phase 1: Context Gathering - Read codebase, write `about.md` and `context.md`. Use Phase 1F for follow-up tasks.
3. Phase 2: Planning - Read context, write detailed `plan.md` with numbered steps grouped into phases.
4. Phase 3: Plan Assessment - Review and refine the plan for correctness, completeness, code quality, and phase sizing.
5. Phase 4: Implementation - Execute one implementation unit per plan phase.
6. Phase 5: Build Verification - Build the project, fix any build errors. Skip if no source code was modified.
7. Phase 6: Code Review Loop - Run review and fix iterations until approved or the iteration limit is reached.
8. Phase 7: Windows Line Ending Normalization - On Windows only, after review passes and before the final summary, normalize LF to CRLF for the text source/config files Codex edited in this task.

Use the phase prompt templates in `PROMPTS.md`.

## Execution Mode

Use Codex subagents as the primary orchestration mechanism.

- Spawn a fresh subagent for context gathering, planning, plan assessment, each implementation phase, and each review or review-fix pass when delegation is available.
- Run Phase 7 in the main session on Windows because it depends on the final local file state and the exact touched-file set for the current task.
- Prefer `worker` for phases that write files. Use `explorer` only for narrow read-only questions that unblock your next local step.
- Keep `fork_context` off by default. Pass the phase prompt and explicit file paths instead of the whole thread unless the phase truly needs prior conversational context or thread-only attachments.
- When the platform supports it, request `model: gpt-5.4` and `reasoning_effort: xhigh` for spawned phase agents. If overrides are unavailable, inherit the current session settings.
- Write the exact phase prompt to the matching `logs/phase-<name>.prompt.md` file before you delegate. Use the same prompt file as a checklist if you later need to fall back to same-session execution.
- After a subagent finishes, verify that the expected artifacts or code changes exist, then write a short result log in `logs/phase-<name>.result.md`.
- Use `wait_agent` only when the next step is blocked on the result. While the delegated phase runs, do small non-overlapping local tasks such as validating directory structure or preparing the next prompt file.
- Build verification is critical-path work. Prefer running the build in the main session, and only delegate a bounded build-fix phase when there is a concrete reason.
- If subagents are unavailable in the current environment, or current policy does not allow delegation for this request, run the phase in the main session using the same prompt files. Never fall back to shell-spawned `codex exec` child processes from this skill.

## Verification Rules

- If build or test commands fail due to file locks or access-denied outputs (C1041, LNK1104), stop and ask the user to close locking processes before retrying.
- Never claim completion without:
  - implemented code changes present
  - build attempt results recorded
  - review pass documented with any follow-up fixes
  - on Windows, if the task edited project source/config text files, a line-ending normalization pass recorded after review

## Completion Criteria

Mark complete only when:
- All plan phases are done
- Build verification is recorded
- Review issues are addressed or explicitly deferred with rationale
- On Windows, Codex-edited project source/config text files have been normalized to CRLF and the result is logged
- Display total elapsed time since start (format: `Xh Ym Zs`, omitting zero components)
- Remind the user of the project name so they can request follow-up tasks within the same project

## Error Handling

- If any phase fails, times out, or gets stuck, inspect the partial artifacts, tighten the prompt, and retry with a fresh subagent or same-session fallback. Report the issue to the user if you remain blocked.
- If `context.md` or `plan.md` is not written properly by a phase, rerun that phase with more specific instructions.
- If build errors persist after the build phase's attempts, report the remaining errors to the user.
- If a review-fix phase introduces new build errors that it cannot resolve, report to the user.
- If Phase 7 cannot safely normalize a touched file on Windows, record the failure in the result log and report it in the final summary instead of silently skipping it.

## User Invocation

Use plain language with the skill name in the request, for example:

`Use local task-think skill with subagents: make sure FileLoadTask::process does not create or read QPixmap on background threads; use QImage with ARGB32_Premultiplied instead.`

For follow-up tasks on an existing project:

`Use local task-think skill with subagents: my-project also handle the case where the file is already cached`

If screenshots are relevant, include file paths in the same prompt when possible.
