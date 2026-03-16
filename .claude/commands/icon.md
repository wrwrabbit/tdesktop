---
description: Generate an SVG icon from a text description with iterative visual review
allowed-tools: Read, Write, Edit, Glob, Grep, Bash, Agent, Task, AskUserQuestion, TodoWrite
---

# Icon - SVG Icon Generation Workflow

You generate production-quality SVG icons for Telegram Desktop through an iterative generate/review loop with visual feedback.

**Arguments:** `$ARGUMENTS` = "$ARGUMENTS"

If `$ARGUMENTS` is empty, ask the user to describe the icon they want.

## Overview

The workflow generates SVG menu icons (24x24, white monocolor outlines on transparent background) matching the Telegram Desktop icon set style. Each iteration is rendered to a PNG for visual review by a fresh subagent that decides whether to approve or improve.

Working directory: `.ai/icon_{name}/` with iterations `a.svg`, `b.svg`, ..., their renders `render_a.png`, `render_b.png`, ..., review notes `b-review.md`, `c-review.md`, ..., and `context.md` tracking the request.

Follow-ups are supported: `/icon {icon_name} make the lines thicker` continues from where the previous run left off.

## Phase 0: Setup

**Record the current time** (using `Get-Date` in PowerShell or equivalent) as `$START_TIME`.

### Step 0a: Follow-up detection (MANDATORY — do this FIRST)

Extract the first word/token from `$ARGUMENTS` (everything before the first space or newline). Call it `FIRST_TOKEN`.

Run these TWO commands using the Bash tool, IN PARALLEL:
1. `ls .ai/` — to see all existing icon project names
2. `ls .ai/icon_{FIRST_TOKEN}/context.md` — to check if this specific icon project exists

**Evaluate the results:**
- If command 2 **succeeds** (context.md exists): this is a **follow-up**. The icon name is `FIRST_TOKEN`. The follow-up description is everything in `$ARGUMENTS` AFTER `FIRST_TOKEN` (strip leading whitespace).
- If command 2 **fails** (not found): this is a **new icon**. The full `$ARGUMENTS` is the icon description.

### Step 0b: New icon setup

1. Parse `$ARGUMENTS` to determine:
   - **Icon description**: what the icon should depict (the full text prompt, possibly with image attachments)
   - **Icon type**: default is `menu` (24x24 menu/button icon). User may specify otherwise.
   - **Target subfolder**: `menu/` by default, or another subfolder if specified.

2. Choose an icon file name:
   - Lowercase letters and underscores only — **NO hyphens**
   - Match existing naming conventions (check `Telegram/Resources/icons/{subfolder}/`)
   - Must NOT conflict with existing icons
   - Must NOT collide with existing `.ai/icon_{name}/` directories

3. Create `.ai/icon_{name}/`.

4. Write `.ai/icon_{name}/context.md` with:
   ```
   ## Icon: {icon_name}
   Type: {menu/other}
   Target: Telegram/Resources/icons/{subfolder}/{icon_name}.svg

   ## Original Request
   {full $ARGUMENTS text}

   ## Follow-ups
   (none yet)
   ```

5. Set `LETTER` to `a`.

### Step 0c: Follow-up setup

1. Read `.ai/icon_{name}/context.md` to get the icon type, subfolder, and full history.

2. Find the latest existing SVG in `.ai/icon_{name}/` (highest letter).

3. Set `LETTER` to the next letter after the latest SVG.

4. Update `.ai/icon_{name}/context.md` — append the follow-up description to the `## Follow-ups` section:
   ```
   ### Follow-up (starting at letter {LETTER})
   {follow-up description}
   ```

### Step 0d: Verify renderer

**Locate the render tool**. The `codegen_style` binary has a `--render-svg` mode that renders SVGs to PNG using the same Qt SVG renderer used at runtime. Find it:

```bash
ls out/Telegram/codegen/codegen/style/Debug/codegen_style.exe
```

If missing, build it: `cmake --build out --config Debug --target codegen_style`

**Test the renderer** on a known good SVG:

```bash
out/Telegram/codegen/codegen/style/Debug/codegen_style.exe --render-svg Telegram/Resources/icons/menu/tag_add.svg .ai/icon_{name}/test_render.png 512
```

If it works, delete the test render and set `RENDER_AVAILABLE = true`. If it fails, set `RENDER_AVAILABLE = false`.

## Phase 1: Iterative Generation Loop

Each run (new or follow-up) allows up to **8 iterations**. Track the starting letter as `START_LETTER`.

Set `MAX_ITERATIONS` to 8. If `RENDER_AVAILABLE` is false, reduce to 3.

### Loop body

Repeat until APPROVED or `LETTER` >= `START_LETTER + MAX_ITERATIONS`:

**Step 1 — Render previous iteration** (skip when LETTER is `a` or no SVGs exist yet)

If there is at least one SVG in the folder, render the latest one:

```bash
out/Telegram/codegen/codegen/style/Debug/codegen_style.exe --render-svg ".ai/icon_{name}/{prev_letter}.svg" ".ai/icon_{name}/render_{prev_letter}.png" 512
```

If rendering fails, set `RENDER_AVAILABLE = false` and proceed.

**Step 2 — Spawn subagent**

Spawn an agent (Agent tool, subagent_type=`general-purpose`) with the prompt built from the **Subagent Prompt Template** below. Pass to it:

- The full context from `.ai/icon_{name}/context.md` (original request + any follow-ups)
- The icon name and current letter
- Whether this is the first generation (no SVGs exist) or a review iteration
- Paths to ALL previous `.svg` iterations in `.ai/icon_{name}/`
- Paths to ALL previous `*-review.md` files
- Path to the rendered PNG of the latest iteration (if `RENDER_AVAILABLE` and SVGs exist)

**Step 3 — Parse result**

The subagent's response will end with either `GENERATED` or `APPROVED` on the last line.

- **GENERATED**: the subagent wrote `.ai/icon_{name}/{letter}.svg`
  - Verify the file exists and contains `<svg`
  - Advance LETTER to next in sequence
  - If iterations used >= MAX_ITERATIONS → break
  - Otherwise → continue loop

- **APPROVED**: the subagent found the latest render satisfactory
  - The final SVG is the **previously rendered** one (`{prev_letter}.svg`)
  - Break out of loop

- **Neither / unclear**: if the new SVG file exists, treat as GENERATED. Otherwise ask the user.

## Phase 2: Output

1. Determine the final SVG file:
   - If APPROVED: the SVG from the letter before the current one (the one that was rendered and approved)
   - If loop ended by max iterations or last GENERATED: the latest written SVG

2. Read the `Target:` line from `.ai/icon_{name}/context.md` to get the output path.

3. Copy the final SVG to that target path (e.g., `Telegram/Resources/icons/menu/{icon_name}.svg`).

4. Update `.ai/icon_{name}/context.md` — append to the end:
   ```
   ## Latest Output
   Letter: {final_letter}
   Written to: {target_path}
   ```

5. Report to the user:
   - Final icon file path
   - Number of iterations taken (in this run)
   - Suggest opening the SVG in a browser to verify visually
   - Mention the working directory `.ai/icon_{name}/` has all iterations and renders
   - Calculate and display elapsed time since `$START_TIME` (format `Xm Ys`)
   - Remind the user they can follow up: `/icon {icon_name} <description of what to change>`

---

## Subagent Prompt Template

Build the subagent prompt by filling in `{placeholders}`. The prompt structure differs slightly for first generation vs review iterations.

~~~
You are an expert SVG icon designer for Telegram Desktop.

{IF no SVGs exist yet (first generation)}
## Task
Generate a NEW SVG icon based on the description below. There are no previous attempts.

{IF SVGs exist (review/improve)}
## Task
Review the latest rendered icon and decide whether it is acceptable or needs improvement.

**Default to APPROVED** unless there is a clear, specific problem. The icon does NOT need to be perfect — it needs to be recognizable, have correct visual weight, and look like it belongs in the Telegram icon set. Minor imperfections are acceptable. If you've seen previous review notes that show the same issues being raised repeatedly, that means the issues likely can't be fixed through iteration — APPROVE and move on.
{END IF}

## Icon Description

{Paste the full contents of .ai/icon_{name}/context.md here — this includes the original request and any follow-up instructions.}

{If user attached an image, add: "Read the attached image at {path} — it shows a reference/mockup of the desired icon."}

## Output Target

- Icon name: `{icon_name}`
- Write new SVG to: `.ai/icon_{name}/{letter}.svg`
- Write review notes to: `.ai/icon_{name}/{letter}-review.md` (only when generating a new iteration, not when approving)

## Style Requirements

You are generating a **{menu/other}** icon for Telegram Desktop. These icons have VERY specific characteristics you MUST match exactly.

### SVG Structure (follow exactly)

The SVG must be minimal — no title, no id attributes, no xlink namespace, no version. These icons are embedded as-is in the app binary, so every unnecessary byte matters.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<svg width="24px" height="24px" viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
    <g stroke="none" fill="none" fill-rule="evenodd">
        <path d="..." fill="#FFFFFF" fill-rule="nonzero"></path>
    </g>
</svg>
```

Do NOT include: `<title>`, `id` attributes, `xmlns:xlink`, `version="1.1"`, or any other metadata.

### Visual Style Rules

1. **White on transparent**: All visible content is `fill="#FFFFFF"`. Background is transparent.
2. **Outline appearance through fill paths**: Icons look like outlined/stroked line drawings, but this is achieved ENTIRELY through filled `<path>` elements — **NOT** through `stroke` attributes. The "outline" effect comes from paths that define both the outer and inner edges of each visible line.
3. **Consistent line weight**: Apparent stroke width is approximately **1.2px at 24x24 scale**. Study the reference icons carefully to match this exactly.
4. **fill-rule**: Use `fill-rule="evenodd"` on the `<g>` wrapper (so overlapping sub-paths create transparent holes — this is how outlines of shapes like circles, rectangles are drawn). Individual paths may use `fill-rule="nonzero"` when appropriate.
5. **Canvas**: 24x24 viewBox with content centered, approximately 2-3px padding from edges.
6. **Recognizable at small size**: The icon is displayed at just 24x24 CSS pixels. It must read clearly.
7. **Professional quality**: Smooth curves, precise coordinates, balanced composition. No jagged edges.

### Technical Rules

- High-precision decimal coordinates (like the references — they use 6+ decimal places)
- Cubic bezier curves (`C`/`c` commands) for smooth curves
- Standard SVG path commands: `M`, `L`, `C`, `Q`, `A`, `Z`
- **NO** `stroke` or `stroke-width` attributes anywhere
- **NO** transforms — bake all positions into path coordinates
- **NO** external references, filters, gradients, or `<use>` elements
- **NO** basic shape elements (`<circle>`, `<rect>`, `<line>`) — use only `<path>` (and `<polygon>` only when trivially simple), matching the reference style
- Multiple `<path>` elements within the `<g>` are fine for icons with distinct visual parts

### Reference Icons (READ ALL OF THESE)

Read ALL of the following SVG files. They define the exact style you must match — study their structure, path complexity, visual weight, and how they achieve the outline appearance:

1. `Telegram/Resources/icons/menu/tag_add.svg` — tag shape with plus sign
2. `Telegram/Resources/icons/menu/tag_edit.svg` — tag shape with pencil
3. `Telegram/Resources/icons/menu/craft_random.svg` — dice with dots
4. `Telegram/Resources/icons/menu/craft_chance.svg` — circle with arrows
5. `Telegram/Resources/icons/menu/craft_start.svg` — forge/hammer tools
6. `Telegram/Resources/icons/menu/reorder.svg` — grid with arrow
7. `Telegram/Resources/icons/menu/rating_refund.svg` — circular arrow + dollar sign
8. `Telegram/Resources/icons/menu/rating_gifts.svg` — gift box
9. `Telegram/Resources/icons/menu/users_stars.svg` — people with stars

Pay close attention to:
- The apparent line thickness and how it's achieved through fill paths
- How shapes have "holes" (e.g., the inside of circles, rectangles) via overlapping sub-paths + even-odd fill rule
- The overall visual density and balance within the 24x24 space

NOTE: The reference icons contain `<title>`, `id`, `xmlns:xlink`, and `version` attributes — IGNORE those. Your output must use the minimal SVG structure shown above (no title, no ids, no xlink, no version).

{IF SVGs exist (previous iterations)}
## Previous Iterations

SVG files (read these to see what was tried):
{For each existing letter:}
- `.ai/icon_{name}/{letter}.svg`
{End for}

Review notes from previous iterations (read these to understand what was changed and why — if the same issues keep appearing, they likely can't be fixed through further iteration):
{For each existing {letter}-review.md:}
- `.ai/icon_{name}/{letter}-review.md`
{End for}

{IF RENDER_AVAILABLE}
## Latest Render

Read this image to see how the latest iteration looks when rendered (white on black, 512x512):
`.ai/icon_{name}/render_{prev_letter}.png`

The render shows the icon upscaled to 512x512 for visibility. Review it:
- Does it match the description? Is the depicted object recognizable?
- Are proportions and visual weight reasonable compared to the reference icons?
- Are there broken paths, artifacts, or major visual glitches?

**APPROVE unless there is a clear, nameable problem.** Do not reject for minor imperfections or subjective style preferences. If previous reviews show the same issue being raised 2+ times, APPROVE — further iteration won't help.
{END IF}
{END IF}

## Your Output

{IF no SVGs exist (first generation)}
Generate the SVG icon and write it to `.ai/icon_{name}/{letter}.svg`.
Briefly describe what you drew and your design approach.
Your LAST LINE must be exactly: GENERATED

{IF SVGs exist (review/improve)}
Either:

a) **APPROVED** — The icon is acceptable (recognizable, correct weight, no major issues).
   Briefly say why it's good enough. Your LAST LINE must be exactly: APPROVED

b) **GENERATED** — There is a specific, clear problem that you can fix.
   First write `.ai/icon_{name}/{letter}-review.md` with:
   - What specific problem you see in the current render
   - What you are changing and why
   - Whether this issue was raised in previous reviews (if so, acknowledge it)
   Then write the improved SVG to `.ai/icon_{name}/{letter}.svg`.
   Your LAST LINE must be exactly: GENERATED

**Bias toward APPROVED.** The icon doesn't need to be perfect. If the object is recognizable and the visual weight is reasonable, approve it. Only reject for clear problems: broken/missing shapes, wrong object depicted, severely wrong line weight, visible artifacts.
{END IF}

CRITICAL: Your absolute last line of output MUST be exactly the word `GENERATED` or `APPROVED` with nothing else on that line.
~~~

## Error Handling

- If the render helper fails, proceed with SVG-code-only review and reduce max iterations to 3.
- If a subagent fails or produces garbled output, retry once with a reminder to follow the format.
- If the subagent writes invalid SVG (missing `<svg` tag), discard it and retry.
- If after all iterations the result is still poor, report to the user and suggest manual refinement or a follow-up with specific feedback.
