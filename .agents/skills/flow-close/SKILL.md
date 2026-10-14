---
name: flow-close
description: >-
  Close an effort after its last task has landed: check the assembled whole
  against the spec, demo it, revisit its decisions, write the retrospective,
  mark the spec done. Use when flow-implement finishes an effort's last task or
  the user says "close the effort", "wrap up <effort>".
argument-hint: "<effort>"
---

# Flow: Close

Read `.workflow/CONVENTIONS.md` §6 (lifecycle) and §7 (review), and `.workflow/STYLE.md` first.

Every task was checked on its own, against its own baseline. Nobody has yet checked that the pieces add up to what the spec promised; tasks that each pass can still fail together. This skill does that check, and an effort is not done until it has.

## 1. Confirm closable

- Find the spec: `.workflow/specs/<effort>.md`. If there is none, stop and say so in one line: an effort without a spec (a lone bugfix, a spike, `maintenance`) is done when its last task lands, with no close and no retrospective.
- No task of `<effort>` is left under `.workflow/tasks/<effort>/`. If one is, name it and stop: it is unfinished, or it should be dropped through `flow-break` amend.
- Every archived task has landed on the main branch (`merge-strategy.md`). Work from an up-to-date main branch.
- If `.workflow/done/<effort>/retrospective.md` exists, this is a reopen (CONVENTIONS §6):
  the tasks added since the last close are what this run closes, and step 6 appends
  instead of rewriting. Use the `Base commit` of the earliest-started added task as
  the comparison point; otherwise use the effort's earliest-started archived task.
- List the effort's commits once and reuse the list:
  `git log --format='%h %s' <point>..HEAD -- .workflow/done/<effort>/`. On a reopen,
  include the newly archived tasks, not earlier closes.

## 2. Run the spec's checks

Run every check in the spec's "How we will know it works", by ID, against the assembled code. These describe the whole; they are not the task Checks again. Record each as pass, fail, or not runnable with the reason. A check no task lists in `Covers` is expected: some only make sense once everything is in. A failing check is a blocking finding.

Then use the feature once the way the spec's "What it does" describes, as a user would, and note what you saw. Step 5 shows this to the user.

## 3. Review the whole

Run `flow-verify` in effort mode (its "Effort mode" section) with the comparison point
and commit list from step 1: it reuses the tasks' `Review boundary` records and runs
the assembly checks, without replaying per-task briefs.

A spec may ask for a separate evidence or documentation pass over the assembled whole;
that pass is an independent invocation, judging the assembly and the evidence, never a
rerun of an unchanged task brief.

Handle findings per CONVENTIONS §7. A fix that passes as a small task (CONVENTIONS §4) is made here, rechecked, and committed with the close. Anything bigger becomes a task in this effort through `flow-break` amend, and closing waits for it.

## 4. Revisit decisions

List the effort's decisions with `grep -n 'Effort:\*\* <effort>' .workflow/decisions.md`, plus any entry the spec cites. For each, read `Revisit when` and ask whether what this effort built or learned has triggered it. A triggered entry goes into the report with what triggered it. Reversing it is a new entry made with the user (CONVENTIONS §2), not here on your own.

List open hotfix follow-ups (`grep -rl 'Hotfix:' .workflow/tasks/`) that touch this effort's code. They stay open until their own task lands.

## 5. Show the user

One message: each spec check by ID with its result, what the demo showed, review status per brief, triggered decisions. Ask whether the effort is accepted.

This is the only stop in this skill. Accepted: carry on. Something missing: it becomes tasks through `flow-break` amend, and closing waits.

## 6. Write the retrospective

`.workflow/done/<effort>/retrospective.md`. The git history of `.workflow/tasks/<effort>/`, `.workflow/done/<effort>/` and the spec answers the plan-versus-actual questions (`git log --stat --follow -- <path>`); the `## Progress` sections of archived tasks hold the review findings.

On a reopen, append a `## Reopened <date>` section with the same headings, covering
the tasks added since the last close, instead of replacing the file: what the effort
originally shipped stays readable.

```markdown
# Retrospective: <effort>

## Goal
<one or two sentences, from the spec>

## What shipped
<what a user can now do; spec check results by ID>

## Decisions that shaped it
<entry headings, one line each on why it mattered>

## Plan versus actual
- Tasks at break: N. Added later: N. Split: N. Dropped: N, and why.
- Spec changes after confirmation: N, which sections, why.
- Tasks that went `blocked`: which, and what the plan had wrong.

## Review findings
<blocking findings by kind: correctness, missing deliverable, standards, process.
A kind that came up in more than one task is a pattern.>

## What changes
<each pattern, and the check or standards.md line it became; or why nothing>

## Debt left behind
<technical-debt.md entries by heading, and open hotfix follow-up tasks>
```

A pattern in the review findings is the most useful line here; a retrospective that changes nothing is a diary. Turn each pattern into something the next effort can't miss:

- A mechanical pattern (banned API, import shape, file location, missing test) becomes a check: a lint rule, a test, or a CI step. Build it in this change if it passes as a small task (CONVENTIONS §4); otherwise draft it as a task.
- A judgement call becomes one line in `.workflow/standards.md` ("How code is written here" or "What we do not do").

## 7. Mark done and commit

1. Spec: `**Status:** done`.
2. `.workflow/bin/workflow validate`. Errors block.
3. One commit: retrospective, spec status, new checks or `standards.md` lines, decision entries, any fixes from step 3. Land it per `Landing:` (`merge-strategy.md`).

## 8. Report

Five lines:
- Whether the effort is done, or exactly what is left
- Spec checks: how many passed, and which failed or couldn't run
- What the effort review found and what was done about it
- Decisions whose `Revisit when` has triggered
- Checks or `standards.md` lines added, or "none"
