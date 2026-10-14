---
name: flow-break
description: >-
  Cut spec into tasks a single session can finish, with dependencies. Use when
  user says "break this down", "split into tasks", or before parallel work.
---

# Flow: Break

Read `.workflow/CONVENTIONS.md` §2 and §2.1 first. Input: a spec (`.workflow/specs/<effort>.md`) or settled conversation. Output: task files in `.workflow/tasks/<effort>/`, archived to `.workflow/done/<effort>/` when finished.

Run it even when the work fits one session: the task file holds the Base commit, the Check and the archive. A one-task breakdown is quick.

It also runs in amend mode when a task is `Status: blocked` or the spec changed under tasks already written (see "Amending an effort").

## Why tasks exist

Long sessions degrade, so each task fits one fresh session with room to spare. Tasks also declare blockers, so independent ones can run in parallel in separate worktrees.

## Good task shape

- **Delivers working behavior** end to end: a narrow path that runs and can be checked. "Add database layer" delivers nothing usable.
- **A fresh session can finish it** from its own file plus the spec sections it names.
- **Checkable** by someone other than its author.
- **Independent** of tasks declared after it.

**Task types** (CONVENTIONS §2.1 owns what each Check proves):
- `feature` (default) and `bugfix`: the Check fails before the change and passes after. A bugfix is built per CONVENTIONS §8: the Check reproduces the bug and `Delivers` says what works again. If nobody knows the cause yet, draft a spike first and block the fix on it.
- `refactor`: behavior must not change; the Check passes before and after.
- `spike`: the deliverable is an answer ("yes", "no", "yes but needs X"), possibly with throwaway code. Follow `.workflow/spike-tasks.md`. Time-box it as a scope limit (one prototype, one benchmark run), not hours.

**Wide refactors.** A mechanical change whose blast radius breaks many call sites at once (rename a column, retype a shared symbol) can't be a vertical slice. Sequence it: expand (add the new form beside the old) → migrate in batches, each a task blocked by the expand → contract (remove the old form), blocked by every batch.

## Sizing

Too big if you can't say what it delivers in one sentence without "and", or it needs more than about a dozen files read to start. Too small if it can't be checked on its own. When unsure, cut smaller.

## Process

### 1. Read spec and code

Find where the work touches. Tidying that makes the real change easier (rename a confusing thing, extract a shared piece, move a misplaced function) goes first as its own task.

Then read `.workflow/technical-debt.md` (or grep it for the modules involved). An entry whose `Where` overlaps this work is a candidate tidy-first task when fixing it makes the change easier; otherwise name it in the hand-off. A task that fixes an entry deletes it in its own commit.

### 2. Draft tasks

For each:
- **Effort** — lowercase slug `[a-z0-9]+(-[a-z0-9]+)*`, the same slug as the spec; it names `tasks/<effort>/` and `done/<effort>/`
- **Task type** — `feature`, `bugfix`, `refactor`, `spike` (optional, default `feature`)
- **Base commit** — the `<commit-sha>` placeholder; `flow-implement` fills it when starting
- **Delivers** — behavior someone can observe once done (spike: "Answer to: <question>")
- **Blocked by** — `None`, or a comma list of `NN` (same effort) or `<effort>/NN`. Numbers only; `validate` rejects titles.
- **Covers** — the spec check IDs this task makes pass, e.g. `W1, W3`. Omit when there is no spec or it covers none.
- **Files** — expected starting locations, not a whitelist
- **Read first** — spec sections, glossary entries, files needed to start. A long list means the task is too big.
- **Check** — command and the result that means it works (spike: how the answer is validated)
- **Done when** — acceptance checklist

Behavior and checks, not code: a snippet only when it carries a decision prose can't (schema, state machine, type). Create tasks only for work someone asked for.

If the user gave a priority, set `**Priority:**` in the spec header.

**Coverage gate.** Every spec check ID appears in some task's `Covers`, or is named in the hand-off as checked only at `flow-close` because it needs all the pieces. Any other uncovered check is a hole in the plan. `workflow validate` reports a `Covers` ID the spec does not define; it cannot see the reverse, so this gate is still yours to run.

### 3. Review round

Present one message:
1. The spec summary in three to five lines: what it's for, what it does, the key decisions, open questions. Skip this when the spec was already confirmed in an earlier session.
2. The breakdown as a numbered list: title, what it delivers, what blocks it. For parallel work, mark each pair as truly independent, sharing an interface ("Coordinate with NN on <interface>"), or easier merged in order ("Easier after NN").

Ask: right size? Dependencies only what truly gates each task? Anything to join or split? One approval settles both the spec and the tasks. Iterate as one round, not a negotiation per task.

A one-task breakdown with an already confirmed spec skips the questions: say what the task delivers and how it's checked, and go on.

### 4. Write files

`.workflow/tasks/<effort>/<NN>-<slug>.md`, numbered `01`, `02` within the effort in dependency order (blockers first). Other efforts reuse numbers, so refer to tasks elsewhere as `<effort>/<NN>`.

```markdown
# <NN>: <Title>

**Effort:** <effort>
**Task type:** feature
**Base commit:** <commit-sha>

**Delivers:** <observable behavior>

**Blocked by:** None | <NN>, <effort>/<NN>
**Files:** <where work happens>

**Read first:** <spec sections, glossary terms, files>
**Covers:** <check IDs>

**Check:** `<command>` → <result that means it works>

- [ ] <acceptance point>
- [ ] <acceptance point>
```

Run `.workflow/bin/workflow validate` and fix every error.

Then commit the spec, decision and glossary entries and task files together, one commit on the main branch, before any task branch is cut (CONVENTIONS §2). Under `Landing: pr` that commit goes through a PR like any change.

### 5. Hand off

Report:
- What can start now
- What runs in parallel (truly independent vs needs coordination), and the merge order if conflicts are likely
- Spec checks left to `flow-close`, and debt entries in the way that weren't made tasks

Each task must be buildable by a fresh session reading only its file and the spec sections it names. If it would need this conversation, fix the task file now.

Build with `flow-implement`, one task at a time per working tree; parallel tasks need separate worktrees. A task whose branch `<effort>/<NN>-<slug>` exists is claimed. A task starts when every blocker is archived under `.workflow/done/<effort>/` and landed (`merge-strategy.md`), or, under `Landing: pr`, stacked on the blocker's branch and recorded as `Base branch`. `next` treats an archived blocker as finished.

## Amending an effort

Runs when a task of the effort is `Status: blocked`, or the spec changed under tasks already written. The plan is fixed here, not worked around in code.

1. Read what changed: the blocked task's `## Blocked` section, and the spec change or decision entry that answered it (`git diff` on the spec). Under `local-merge` and `pr` the blocked task file is on its branch: `next` names it, `git show <branch>:<path>` reads it.
   If the spec says `done` — a task joined a closed effort — set it back to `**Status:** in progress` as part of this amend (CONVENTIONS §6).
2. Go through every remaining task under `.workflow/tasks/<effort>/`, the blocked one first: does `Delivers` still match the spec? Do `Read first`, `Files`, `Check` and `Covers` point at the right things? Is `Blocked by` still the real gate?
3. Rewrite what is stale. Delete a task with no reason left to exist: name it and why in the decision entry for the change, and fix every `Blocked by` that pointed at it. New tasks take numbers after the highest the effort has used, archived ones included; never renumber.
4. Edit the blocked task from its branch copy, so `Base commit`, `Started` and `## Progress` reach the main branch with it, and remove `## Blocked`. Then take one of two branches:
   - **What it delivers is unchanged.** Set `Status: paused`, and make sure `## Progress` says where the earlier work is, so `flow-implement` resumes on the same Base commit.
   - **What it delivers changed.** Reset `Base commit` to the `<commit-sha>` placeholder, drop `Started` and `Status`, name reusable work in `## Progress`, and rename the old task branch to `attempt/<effort>-<NN>` so the task can be claimed afresh.
5. Rerun the coverage gate.
6. Present the changes as one round, as in step 3: what was rewritten, added, dropped, and why.
7. Run `.workflow/bin/workflow validate`, then commit the spec, decision entries and task changes together on the main branch, as in step 4.
