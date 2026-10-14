---
name: flow-implement
description: >-
  Build one task end to end: code, proof, docs, review, archive, commit, land. Use
  when user says "implement task 3", "build this", "do next task". Levels: standard (default) or thorough.
argument-hint: "<effort>/<NN> [thorough]"
---

# Flow: Implement

Read the task file and the spec sections it names, and CONVENTIONS §6 (pause
handoff), §7 (review) and §8 (fixing bugs). Read `.workflow/STYLE.md` when writing
the commit message and the report. A task file that needs the conversation that
produced it is broken; fixing it is part of this job.

Find facts in the code. Work end to end; stop only for a decision that changes what
gets built (one batch of questions) or when the plan is wrong (below). Bare `NN` is
fine when only one effort has that number; otherwise ask which.

Levels: `standard` (default) runs `flow-verify`; `thorough` adds `flow-architect` on
the touched area, for large refactors and code with many callers.

## 1. Start

`Task type: spike`: also follow `.workflow/spike-tasks.md`. `bugfix`: CONVENTIONS §8
governs the build.

Under `local-merge` and `pr` a started task's file lives on its branch (`next`
names it): check that branch out first and read the task file there. If
`flow-break` amend has since changed the task on the main branch, rebase onto the
main branch and keep the main branch's copy of the task file.

`Status: blocked`: say so and stop. `Status: paused`: resume from `## Progress`,
restore what it names (branch, worktree, stash), keep its Base commit and
`Dirty at start`, set `Status: active`, continue from the next step it gives.

For a new task, the task file must be committed on the main branch. When resuming,
use the committed task file on the claimed task branch; do not stop merely because
it has not reached main. In either case, every task in `Blocked by` must be archived
under `.workflow/done/<effort>/` and landed (or this task stacked on its branch).
Otherwise name what's missing and stop.

For a new task, run `git status --porcelain` and record its paths in `## Progress`
as `Dirty at start: <paths>` (or `none`). They belong to someone else and stay out
of every commit; if the task needs one, ask. On resume, keep the original record;
the task's own work named in the handoff is not someone else's dirty work.

Read `Landing:` in `.workflow/standards.md`. Under `local-merge` and `pr`, claim and
create the branch `<effort>/<NN>-<slug>` as `.workflow/merge-strategy.md` describes;
if someone else holds it, say whose and stop. Under `direct`, say in one line that
work goes onto the current branch.

New start: write `**Base commit:** <git rev-parse HEAD>`, `**Started:** <ISO 8601>`
and `**Status:** active` before touching code. Under `pr`, commit and push this task
file before touching code so the claim is visible. Resume: keep the recorded SHA and
confirm `git cat-file -e <sha>^{commit}`; if it fails, stop and report.

`.workflow/specs/<effort>.md` says `proposed`, or says `done` (a later task joined a
closed effort, CONVENTIONS §6): set it to `**Status:** in progress`. Then note in
`## Progress` that the effort was reopened, unless this is its first task.

Fill in where the work goes or how to check it from spec and code if the task
doesn't say, and say so. Read the code and tests it touches; build the way the
project already does. Run the test command from `standards.md` for a clean baseline.

Run the task's `Check`; record `Check before: <output, or why it can't run>` in
`## Progress`:
- `feature`, `bugfix`: fails or can't run yet. If it passes, the work is done or the
  Check misses the behaviour; find out which and fix the Check (say so).
- `refactor`: passes now and must pass after; behaviour stays the same.
- `spike`: skip.

## 2. Build it

Make the smallest change that delivers the task. Named files are starting points:
add the tests, callers and docs the behaviour needs. Add only the configurability,
error handling and abstractions the task asks for; name anything you skipped in the
report. Run typecheck and touched tests as you go, and make the `Check` exist while
writing the code. A bugfix follows CONVENTIONS §8's order. A task that fixes an
entry in `.workflow/technical-debt.md` deletes that entry in its commit.

## 3. Prove it

Run the Check and record `Check after:`. Exercise the real path once (open the UI,
run the command) and note what you saw, including anything odd. If the Check can't
pass, say what blocks it and keep the Check as strict as it is.

## 4. Sync documents and record decisions

Before review, so the review sees them:
- Fix any fact in the spec, glossary or `standards.md` the code no longer matches.
- Put what the task taught into spec, glossary or decisions; the archived task file
  doesn't replace the spec.
- Log only choices that qualify under CONVENTIONS §1.2, in the format in
  `.workflow/decisions.md`, tagged `**Effort:** <effort>`. Note the ones you made
  yourself (`Mine: yes`) for the report.

## 5. Review it

Run the CONVENTIONS §7 security pass when its trigger applies; otherwise say so in
one line. It covers the boundary closure, not the whole change set.

Run `flow-verify` with the task's exact Base commit and task file path. `thorough`:
then `flow-architect` on the touched area. Append each architect finding you don't
act on to `.workflow/technical-debt.md` under `## Entries`: `## <concrete symptom>`
with `Where`, `Added`, `Priority`, `Problem`, `Impact`, `Solution`, `Cost`.

Record the round in `## Progress`: the `Review boundary` line the reports name, and
the brief statuses and findings.

Handle findings per CONVENTIONS §7: fix this round's findings together and recheck
once. A new round is for a `blocking` finding or a fix that moves the boundary, and it
takes every such fix at once. If a fix changes what the task delivers, update the task
file and rerun the affected brief.

## 6. Archive, commit, land

1. Close the task file: tick finished items, add `**Finished:** <ISO 8601>`, remove
   `**Status:**`, keep `## Progress`.
2. `git mv .workflow/tasks/<effort>/<NN>-<slug>.md .workflow/done/<effort>/`.
3. Run `.workflow/bin/workflow validate`. Errors block the commit.
4. Stage the task's paths by name (`git add -- <paths>`), never `Dirty at start`
   paths. One commit: code, tests, docs, decisions and the archive move.
5. Land it per `Landing:` (`.workflow/merge-strategy.md`). Push or open a PR only
   under `pr` or when the user asks.
6. Last task of the effort, all landed: if `.workflow/specs/<effort>.md` exists, go
   to `flow-close` (here if the window is light, else in a fresh session,
   `.workflow/phase-boundaries.md`). Without a spec, the effort is done.

Spike: findings go into the task's `## Findings` and into spec, decisions or
glossary. Throwaway code stays off the main branch: drop it, or keep it on
`spike/<effort>-<NN>` named in Findings. Then archive, commit and land as above.

## 7. Report

Unfinished task: say so first, say what remains, leave it unarchived (pause it).

Regular task, five lines: what now works for the user; what proved it (Check before
and after, real-path run); what review found and what you did; what you left out and
when it would matter; decisions made on the user's behalf (`Mine: yes`) by heading,
or "none".

Spike, five lines: the question; the finding; where it is recorded; code kept (which
branch) or discarded; decisions made on the user's behalf, or "none".

## When the plan is wrong

The task or spec assumes something the code doesn't do, contradicts itself, or is
really several tasks: fix the plan, not the build.

1. Set `**Status:** blocked` and add `## Blocked`: what was assumed, what is true
   (file, command, output), what has to be decided.
2. Leave partial code on the task branch or in a stash named
   `<effort>/<NN> blocked`, and say where.
3. Commit the task file on its own: on the task branch under `local-merge` and
   `pr` (push it under `pr`), where `next` and `flow-break` amend find it.
4. Report and route per CONVENTIONS §5. The task resumes once `flow-break` amend
   clears `blocked`.

## Pausing and resuming

When the session stops before the task is done, write the CONVENTIONS §6 handoff
into `## Progress` and set `**Status:** paused`. A session that never saw this
conversation must be able to continue from the task file alone.

## Scope

Build the task as written. Report unrelated problems as candidate tasks. Decisions
in `.workflow/decisions.md` stay settled.
