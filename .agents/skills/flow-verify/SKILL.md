---
name: flow-verify
description: >-
  Review change with two briefs of the workflow-reviewer agent: built as asked
  (with evidence and process), and code quality. Use before merging or when user
  says "review this".
---

# Flow: Verify

Read `.workflow/CONVENTIONS.md` §2 and §7 first.

Two briefs, each its own `workflow-reviewer` invocation, so neither review sees the
other's conclusions:

1. **Built as asked, and process** — requirements met, evidence that it works,
   documents and decisions still true, leftovers, scope.
2. **Quality** — correctness, callers, maintainability.

This is the review behind every `flow-implement` level (`thorough` adds
`flow-architect` after it). `flow-close` runs it in effort mode (below).

## 1. Pin comparison point

From `flow-implement`: the task's exact Base commit, recorded before implementation.
Never fall back to `main`.

Standalone: whatever the user named (commit, branch, tag). If nothing, use `main`
(or `origin/HEAD` if there is no `main`), say you assumed it, let them correct it.

After a rebase (landing, `merge-strategy.md`), `git diff <Base commit>` also
contains everything others landed in between. Compare against
`git merge-base <main branch> HEAD`, or the stacked `Base branch`, and say so. The
Base commit stays in the task file as history.

Capture the diff commands once and reuse them:
```sh
git diff <point>                          # working tree vs comparison point
git diff <point>...HEAD                   # committed changes since point
git diff HEAD                             # uncommitted vs HEAD
git ls-files --others --exclude-standard  # untracked
git log <point>..HEAD --oneline           # commit history
```

Confirm the point resolves to a commit and the combined change set (committed,
staged, unstaged, untracked) is non-empty before spawning agents. From
`flow-implement` the work is usually still uncommitted; that is fine. A bad point or
an empty change set fails here, not inside the agents.

## 2. Gather what each brief needs

**Brief A** needs the source of truth to judge requirements against. From
`flow-implement` the task file path is passed in; hand it to the agent. Otherwise
look in commit messages, `.workflow/specs/`, `.workflow/tasks/`, or the path the
user gave. With none, run the brief as "no spec available" rather than inventing
requirements. It also reads `.workflow/` for documents, decisions and debt.

**Brief B** needs `.workflow/standards.md` plus repo conventions (`AGENTS.md`,
`CONTRIBUTING.md`, lint config).

## 3. Spawn review agents

| Brief | Agent role | Needs |
| --- | --- | --- |
| A. Built as asked, and process | `workflow-reviewer` (brief A) | diff commands, untracked files, commit list, task file and spec, `.workflow/` |
| B. Quality | `workflow-reviewer` (brief B) | diff commands, untracked files, commit list, `.workflow/standards.md`, repo conventions |

Give each agent the diff commands, untracked file list, commit list, and the task
file and spec paths; they run the commands themselves. Name the brief in the prompt
so the agent knows which half of its instructions applies. Their `blocking` /
`nonblocking` labels stand (CONVENTIONS §7). Ask each agent to name the boundary it
reviewed: comparison point, paths, deliverable and trust boundaries. CONVENTIONS §7
records that line in `## Progress`.

If the named agent isn't available in this harness, use a general-purpose subagent
with the brief pasted in (`.agents/agents/workflow-reviewer.md`), and say in the
report that you fell back.

## 4. Report

Each brief gets a status:
- `pass`: no finding the reviewer marked `blocking`
- `blocked`: one or more `blocking` findings
- `not-run`: the brief couldn't be evaluated (missing baseline or prerequisite)

One section per brief with the findings as the agent wrote them, lightly cleaned.
Then one line per brief: status, number of findings, worst one. No overall verdict
averaged across briefs. A clean brief is reported as `pass`, not left blank.

End with one line: ready or not, and if not, the shortest path. Any `blocked` or
`not-run` brief means **not ready**.

## 5. Then what

Fix the findings or hand them to the user, per CONVENTIONS §7 (waivers, the boundary,
rechecks sized to the fix, findings kept). One round, one boundary: fix every finding
this round produced, from both briefs, together, then recheck once. Rerun a brief only
when a fix moves the boundary — new caller, new file, new trust boundary, changed
deliverable — and rerun it once for all such fixes. The reviewed diff must match the
change being released: if fixes alter scope, update the task and rerun the affected
brief.

Called from `flow-implement` or `flow-close`: return to it for its remaining steps.

## Effort mode

`flow-close` runs this review over a finished effort. What changes:

- **One brief.** Each task was already reviewed for quality when it landed, so run
  brief A only. For a large effort the user may ask for brief B as well.
- **Reuse the task records.** Each task's `## Progress` holds its `Review boundary`
  and findings; a task whose boundary is unchanged is already reviewed, so do not
  rerun its briefs. Open only commits no brief covered, code changed after its task's
  review, and the assembly checks.
- **The assembly checks always run**, whatever the task records say:
  - every spec check ID, pass / fail / not runnable, plus "What it does" and "What it
    must not do";
  - the integration diff from the effort's true base — the earliest Base commit, or
    the merge-base with the main branch — not each slice's own base;
  - the interfaces and callers between tasks, and generated, config, schema and API
    compatibility across the whole;
  - the trust-boundary inventory over the aggregate diff; the CONVENTIONS §7 security
    pass runs when it is non-empty.
- **Comparison point:** use the point and effort commit list from `flow-close`
  step 1. Other efforts may have landed since: judge the listed commits, not
  everything in the range.
- **Brief A** is judged against the spec ("What it does", "What it must not do", the
  W-numbered checks), not one task. A behaviour the spec promises that no task
  delivered is the finding this mode exists for. The agent also checks that spec and
  glossary match the finished code, and that decisions tagged with the effort are
  honoured or superseded.

The working tree is clean, so the diff is committed history only; a non-empty
commit list is the precondition instead of a non-empty working-tree diff.
