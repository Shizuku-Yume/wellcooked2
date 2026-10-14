---
name: flow-start
description: >-
  Load project context and classify request. Routes trivial questions, small tasks,
  complex work to right path. Use at session start or when resuming work.
---

# Flow: Start

Load the project's context, classify the request, and route it. Runs once per session, or when context was lost.

## 1. Load project context

Read silently:
1. `.workflow/standards.md` — commands, layout, project conventions. If it still holds unfilled `<...>` placeholders, inspect the repository's build, test and lint configs (`package.json`, `pyproject.toml`, `Cargo.toml`, `Makefile`, CI workflows) and top-level layout, fill in `.workflow/standards.md`, and note what you filled in for confirmation in step 5.
2. Check that `.workflow/glossary.md` and `.workflow/decisions.md` exist, but do not preload them. After classification or once the area to touch is known, grep relevant glossary and decision terms; read the newest decision entries and any linked supersession entries as needed.

A missing file goes in the report; a project without decisions or glossary is new, not broken. This keeps the session entry point small while preserving the evidence-before-questions rule.

## 2. Check workflow state

- `.workflow/bin/workflow next --no-color` — ready tasks ranked by priority, and above them every started task: paused, needing replanning, in progress, or finished but not landed, with the branch it lives on. Under `Landing: pr`, `git fetch -q` first so teammates' task branches show. Read `.workflow/tasks/*/` files directly when you need more.
- A paused task is interrupted work: offer to resume it before starting anything new. Under `local-merge` and `pr` its handoff is in the task file on its branch, so check out that branch first.
- `Status: blocked` is a plan waiting to be fixed (CONVENTIONS §5).
- `grep -rl 'Hotfix:' .workflow/tasks/` — hotfix follow-up tasks still waiting for the proper fix (CONVENTIONS §4).
- `grep -n 'Priority:\*\* fix-now' .workflow/technical-debt.md` — debt marked fix-now.

Hotfix follow-ups and fix-now debt don't pick the next task; they go in the report so they aren't forgotten.

If this session will cross several phases, or context is already heavy, follow `.workflow/phase-boundaries.md` at each boundary.

## 3. Classify request

Sort the message into exactly one class from CONVENTIONS §4 (trivial, hotfix, small, complex). Read the criteria there. Every hotfix criterion must hold; between small and complex, choose complex.

The class is provisional: before calling something small, read the callers of what it changes. The CONVENTIONS §4 tripwires upgrade it while building.

## 4. Route

### Trivial → answer directly
Answer or clarify. No other skill.

### Hotfix → fix now, follow-up before the session ends
State the outage you understood and fix it, with no grilling or spec. Verify where the failure was observed; if you can't reach production, say so and hand that check to the user. Before the session ends, write the hotfix decision entry and draft the follow-up bugfix task, both as CONVENTIONS §4 specifies. If you can't write the entry honestly, it wasn't a hotfix: reclassify.

### Small task → fast path
Run `git status --porcelain` first: paths already modified or untracked belong to someone else (CONVENTIONS §2). Then:

- **No decisions needed:** say what you understood and build.
- **User-owned decisions:** ask in one message with your recommendation, wait, then
  build. Log only choices that qualify under CONVENTIONS §1.2.
- **Routine choices:** use the existing convention; several routine choices do not
  make the work complex.

The fast path keeps what CONVENTIONS §4 requires of small tasks (read the code and its callers, tests before and after, proof by running it, CONVENTIONS §8 for a bug, the CONVENTIONS §7 security pass). Land it per `.workflow/merge-strategy.md`.

### Complex → full workflow
Route to the earliest incomplete step with the table in CONVENTIONS §5, using `next` output and existing `.workflow/specs/`, `.workflow/maps/`, `.workflow/tasks/`. Name tasks `<effort>/<NN>`.

## 5. Report and proceed

Three lines:
1. Context loaded (or missing), plus paused or blocked tasks, open hotfix follow-ups and fix-now debt when there are any
2. Classification and why
3. Next action

Then do that action in the same turn. Stop only when the classification genuinely needs the user's input.

## When to skip this skill

Skip it mid-session with context loaded, when the user names a flow skill directly, or when resuming with context still present. Use it on a fresh session, after compaction, when the user asks "what's next", or when unsure which skill applies.
