# Workflow Conventions

Shared rules for all `flow-*` skills. Each skill names the sections it needs;
read those, not the whole file.

**Precedence.** When rules disagree: the user's current instruction, then
`.workflow/standards.md` and the repository's existing conventions (commit style in
`git log`, lint and formatter config, CONTRIBUTING), then this file, then `STYLE.md`.

---

## 0. Evidence before questions

Before asking, check whether the codebase answers it. Current behaviour, patterns,
APIs, constraints, dependencies and configuration are yours to find: `grep`, read,
dispatch subagents. Ask the user only what they own: intent, preference, scope, risk
tolerance, acceptance criteria, and the choice between options that all work.

---

## 1. Decision protocol

### 1.1 Facts are yours to find
§0 applies: only decisions the user owns may interrupt them.

### 1.2 Triage before asking
Every open question goes into one bucket:

**A - Look it up** — facts, conventions, prior decisions (§2 says how to read them),
what APIs do.

**B - Decide it, log if it qualifies** — you can name the clearly better option, or
it has no real stakes. Write a decision entry when all three hold: the choice is
hard to reverse, someone reading the code later would wonder why, and a real
alternative lost. Missing any one, decide it silently and move on; routine naming
and formatting never qualify.

**C - Ask the user** — the answer changes what gets built and the trade-off is real.

### 1.3 Ask in batches
One round holds every question you can ask now without inventing answers to other
questions, at most 6. Format:
```
Q1 - <title>
   <question, 2-3 sentences>
   Options: A | B | C
   My pick: <one>, because <one line>
```

Rank by what blocks the most work. A question needing more than a screen hides a
smaller one.

### 1.4 Confirm before proceeding
Confirm unresolved Bucket C choices. Answers the user already gave are settled;
do not add a second approval just to recap them. Routine Bucket B choices need no
approval.

### 1.5 Stop when unblocked
Stop grilling when no open decision would change the next step.

---

## 2. File structure

```
AGENTS.md                        workflow block points here
.agents/skills/<name>/SKILL.md   the steps
.agents/agents/<name>.md         review agents
.workflow/
  bin/workflow                   the CLI, committed with the project
  bin/workflow-core.py           validate and next (python3, stdlib only)
  CONVENTIONS.md                 this file
  STYLE.md                       writing rules
  standards.md                   project-specific: commands, layout, Landing
  glossary.md                    vocabulary
  decisions.md                   choices made, with runner-up
  technical-debt.md              architecture findings to address
  merge-strategy.md              branches, landing, claiming tasks
  phase-boundaries.md            continue, clear, compact or delegate
  spike-tasks.md                 exploratory work guidelines
  specs/<effort>.md              what to build, for an effort that has a spec
  tasks/<effort>/<NN>-<slug>.md  one task each, in its effort's directory
  maps/<slug>.md                 multi-session planning
  done/<effort>/                 archived tasks, plus retrospective.md written by flow-close
```

**Efforts and specs.** An effort is the slug that groups tasks (`tasks/<effort>/`,
`done/<effort>/`). Its optional spec is `specs/<effort>.md`, with this header:
```
**Status:** proposed | in progress | done
**Priority:** critical | high | normal | low
```
`Priority` is optional (default `normal`) and lives only in the spec header: `next`
ranks an effort's ready tasks by it, and a `Priority` on a task file does nothing
(`validate` says so). `urgent` is accepted as another name for `critical`. A typo
ranks as `normal`, so `validate` warns about a value it does not know. A lone bugfix,
spike or `maintenance` work needs no spec. Update a spec in place when code changes
its facts.

**Rules:**
- Run the CLI as `.workflow/bin/workflow <command>` from the project root. It is
  committed and needs nothing on PATH, so it works in every clone, CI and cloud agent.
- Read the files directly. Active work: `.workflow/bin/workflow next` (ready tasks by
  priority, plus paused and blocked ones) or `.workflow/tasks/`. Prior decisions: the
  newest entries at the end of `decisions.md`, plus `grep -n -i '<term>'
  .workflow/decisions.md` for the area you will touch (`grep -n 'Effort:\*\* <slug>'`
  for one effort). Debt: read or grep `technical-debt.md`.
- `standards.md` is the entry point. Keep it under one screen.
- **Planning documents get committed.** A skill that writes decisions, glossary,
  specs, maps or tasks commits them when it finishes, onto the main branch, before any
  task branch is cut; under `Landing: pr` they go through a PR. A task starts only once
  its task file is committed on the main branch.
- `decisions.md` defines its entry format at the top (five fields: `Decided`, `Instead
  of`, `Because`, `Mine`, `Revisit when`). Cite an entry by heading: `decisions.md:
  <date> - <title>`. The file is strictly append-only with one exception: a new entry
  that reverses an old one carries `**Supersedes:** <old heading>`, and the same change
  appends `**Superseded by:** <new heading>` to the old entry. A superseded entry is
  history: follow the pointer.
- `decisions.md` and `glossary.md` use git's union merge (`.gitattributes`), so
  parallel branches adding entries don't conflict.
- `technical-debt.md` is a flat list of findings, cited by heading, each kept until
  fixed or declined (§6).
- `merge-strategy.md` defines landing (`Landing:` in `standards.md`), claiming a task,
  and when dependents may start.
- **Dirty files at start.** When a task starts, run `git status --porcelain`: paths
  already modified or untracked belong to someone else. Record them in `## Progress`
  as `Dirty at start: <paths>` (or `none`) and keep them out of every commit; stage
  the task's own paths by name (`git add -- <paths>`). If the task needs one, ask.

### 2.1 Task files

`flow-break` drafts them, `flow-implement` builds and archives them. This is the
grammar `validate` enforces; `flow-verify`, `flow-start` and the planning skills need
the paths above, not this.

- Filename `tasks/<effort>/<NN>-<slug>.md`, heading `# <NN>: <Title>`. `NN` is unique
  within an effort; cite a task as `<effort>/<NN>`, at least two digits. Files
  directly under `tasks/` (pre-2.3 layout) still work; `git mv` them when convenient.
- Fields are `Name: value` lines, plain or bold (`**Effort:** <slug>`).
- Required (`validate` errors): `Effort` (slug `[a-z0-9]+(-[a-z0-9]+)*`, same as the
  directory); `Blocked by` (`None`, or a comma list of `NN` or `<effort>/NN`,
  numbers only); `Check` (command and the result that means it works, or a
  `## Check` section).
- Expected from `flow-break`, not machine-checked: `Delivers`, `Files`, `Read
  first`, and the `Done when` checklist.
- `Task type:` (default `feature`) sets what the Check proves. `feature`, `bugfix`:
  it fails (or can't run yet) before the change and passes after. `refactor`:
  behaviour stays the same; it passes before and after, and review confirms no
  behaviour changed. `spike`: no Check-before; the deliverable is an answer
  (`spike-tasks.md`).
- Optional: `Base commit:` (placeholder when drafted, real SHA when started);
  `Started:`/`Finished:` (ISO 8601); `Status: active | paused | blocked` (§6);
  `Covers:` spec check IDs (`W1, W3`); `Base branch:` the unlanded blocker branch it
  was stacked on (`merge-strategy.md`).
- Sections: `## Progress` (dirty paths at start, Check result before the change,
  root cause, review findings and outcomes, pause handoff), `## Blocked` (§5),
  `## Findings` (spike answers).
- Inside `done/<effort>/` the same problems are only warnings.
- Archive the task to `done/<effort>/` in the same commit as its code and docs. A
  blocker archived in HEAD is finished: its dependents become ready unchanged.

After editing tasks or decisions, run `.workflow/bin/workflow validate`. Errors
block; fix the file, not the validator.

---

## 3. Writing

`.workflow/STYLE.md` covers everything a person reads: chat, documents, commits, comments,
UI text. The repository's own conventions win over it (Precedence, above).

---

## 4. Task sizing

**Trivial** — question, clarification, greeting. Answer directly.

**Hotfix** — all of these hold, else it is small or complex: users are affected by a
degraded or down system now; the fix is localized and well understood; waiting is
riskier than skipping process.

Hotfix path: skip grill, spec, break and verify; fix now and verify where the failure
was observed, or hand that check to the user. The
security pass (§7) still runs. Before the session ends, write both:

1. A decision entry in `.workflow/decisions.md`, headed `## <date> - Hotfix: <title>`:
   - **Decided:** the temporary fix applied
   - **Instead of:** the proper solution (or "unclear, needs investigation")
   - **Because:** why it couldn't wait
   - **Mine:** yes
   - **Revisit when:** the proper fix lands

   If it can't be written honestly, it wasn't a hotfix: reclassify.
2. The proper fix as a `Task type: bugfix` task (§8) in the effort owning the code,
   else `maintenance`, with `Read first` citing the hotfix entry by its heading. That
   open task is the tracker: `grep -rl 'Hotfix:' .workflow/tasks/` finds every open one,
   and `flow-start` reports them until they land.

**Small task** — all of these hold: one localized behaviour change or bug fix;
requirements clear; no architectural decisions; revertable in under 5 minutes. Impact
radius matters more than file count: a core type in 1 file used by 20 modules is not
small. When unsure, it is complex.

Small tasks skip `flow-grill`, `flow-spec`, `flow-break` and `flow-verify`, and still
need reading the code, tests, proof and a commit message. Run the dirty-file check
(§2) first. A bug follows §8; the security pass (§7) still runs. Work without a task
file (small task or hotfix) lands as `merge-strategy.md` says.

The first classification is a guess. Before building, read the callers of what you
will change. Upgrade to complex when the change breaks a public contract, persisted
data or an architectural boundary, or when a new decision changes what gets built.
Do not upgrade merely because the change touches another module or uses a routine
dependency; impact and reversibility decide.

Stop, name the reason, keep the work (uncommitted or a named stash) as input to the
plan, and route through §5.

**Complex** — everything else: the full workflow from the earliest incomplete step.

---

## 5. Skill routing

This is the only routing table; `flow-start` and the skills point here.

| Situation | Skill |
|-----------|-------|
| Session start, context lost, or next step unclear | `flow-start` |
| Vague requirements, design questions, or "grill me" | `flow-grill` |
| Too foggy to plan; questions depend on unanswered questions | `flow-map` |
| Decisions settled, need writing down | `flow-spec` |
| Spec or settled conversation needs tasks (even one) | `flow-break` |
| Bug neither small nor a hotfix | `flow-break`: one `bugfix` task, behind a spike if the cause is unknown (§8) |
| Task file ready to build | `flow-implement` |
| A task is `blocked`, or the spec or task proved wrong | replan (below) |
| Review finished work | `flow-verify` |
| Last task of an effort with a spec has landed | `flow-close` |
| Code expensive to change | `flow-architect` |

When two apply, take the earliest incomplete. The user may skip ahead, but complex
work is always built from a task file: it carries the Base commit, Check and archive.

**Who starts a skill.** `flow-start`, `flow-grill`, `flow-map`, `flow-spec`,
`flow-break`, `flow-implement` and `flow-verify` may be entered when the routing
table sends you there. `flow-close`, `flow-architect` and `flow-break` amend change
records other sessions read, so start them only when the user asks, or say in one
line that you are starting one and why before you do.

**Replan.** A plan that turns out wrong is fixed in the plan, not worked around in
code. The task that hit it gets `Status: blocked` and a `## Blocked` section
(`flow-implement` writes both). Then `flow-grill` if the fix needs a user decision,
otherwise update the spec directly (with a decision entry per §1.2); then `flow-break`
in amend mode reviews every remaining task of the effort, since all may be stale.

---

## 6. Lifecycle

Every status has one owner, who writes it in the same change as the work that
justified it.

| Object | States | Who moves it |
|--------|--------|--------------|
| Map `maps/<slug>.md` | `charting` → `working` → `clear` | `flow-map`: `working` once the first question is answered, `clear` when no question or fog is left |
| Spec `specs/<effort>.md` (the effort's state) | `proposed` → `in progress` → `done` | `flow-spec` writes `proposed`; `flow-implement` sets `in progress` when the effort's first task starts, and sets it back from `done` when a later task joins a closed effort; `flow-close` sets `done` |
| Task `tasks/<effort>/<NN>-<slug>.md` | drafted → `active` → (`paused` or `blocked`) → archived | `flow-break` drafts it (no `Status`); `flow-implement` sets `active` on start, `paused` when the session stops early (handoff below), `blocked` when the plan is wrong (§5); `flow-break` amend clears `blocked`; `flow-implement` archives it and drops `Status` |
| Debt entry in `technical-debt.md` | listed → removed | `flow-architect` (or any skill that finds one) adds it; the fixing task deletes it in its commit; if the user declines, delete it and append a decision entry (`Decided: not fixing <symptom>`, `Mine: no`) so reviews don't re-propose it |

An effort without a spec has no status: it is done when its last task lands, with no
`flow-close`. Under `local-merge` and `pr` a started task's file, with its `Status`,
`## Progress` and `## Blocked`, lives on the task branch until it lands; `next` reads
task branches, so claimed, paused and blocked tasks show from the main branch
(`merge-strategy.md`). `next` never offers a started task as ready; resume a `paused`
one before starting anything new.

**Reopening a closed effort.** An effort whose spec says `done` can still gain work:
a follow-up, a bug the effort caused, a fix too big for the close. Adding a task to
it sets the spec back to `in progress` in the same commit, so no task is ever open
under a settled spec. The next `flow-close` runs the new tasks' checks and appends a
`## Reopened <date>` section to the retrospective instead of rewriting it; the
history of what the effort originally shipped stays readable. A large piece of
follow-up work is a new effort instead, with its own spec.

**Pause handoff.** The resuming session never saw this one. Before setting
`Status: paused`, `## Progress` says: what is done, the next concrete step, open review
findings, the `Review boundary` line, the branch or worktree, and where uncommitted
work is (in the tree, or a stash named `<effort>/<NN> paused`). Under `local-merge` and
`pr`, commit the task file alone on its branch so `next` can read it; under `pr`, push
it. A next session that would need this conversation means the handoff is incomplete.

---

## 7. Review

Shared by every review path (`flow-verify`, `flow-implement`, `flow-close`, the fast
paths); skills point here. What to look for lives in the `workflow-reviewer` brief.

**Security pass.** Runs when the change touches a trust boundary: authentication or
authorisation, credentials, untrusted input reaching a shell, query, path, URL or
process, a listener or egress, or permission state. Untrusted means outside this
project's own code and configuration. Review the boundary closure — the changed lines
and the changed functions' direct callers and callees at those boundaries — not the
whole change set. Name the boundaries reviewed, or say in one line that none applies.
Use a `security-reviewer` agent when the harness has one, else a subagent with
`.agents/agents/security-reviewer.md`.

**The reviewer classifies** each finding `blocking` or `nonblocking`; that label
stands.

**Only the user waives a blocking finding.** It is fixed, or the user accepts it in a
decision entry with `Mine: no`. Declining a nonblocking finding needs a `Mine: yes`
entry only when the choice qualifies under §1.2.

**The round has a boundary.** A round covers one change set: a comparison point, its
paths, and the deliverable and trust boundaries it touches. The reviewer names that
boundary in its report; it goes into `## Progress` as
`Review boundary: <point> + <paths>; round: N; briefs: A,B,sec; boundaries: <names|none>;
findings: n blocking / n nonblocking`. A new comparison point — a rebase, a landing, an
integration merge, a resumed task — starts a new round.

**Inside or outside.** A fix is inside the boundary when the point, the paths and the
deliverable are unchanged and it adds no caller, sink or trust boundary. Anything else
is outside and moves the boundary.

**Fixes get rechecked.** Rerun the affected check and the check the finding came from.
A fix inside the boundary needs no new round. One delta round with the same role,
batched with every other open finding from that boundary, covers a `blocking` finding
and any fix that moves the boundary (the security pass re-runs with it) — never a
nonblocking finding, and never a routine change because of line count.

**Findings are kept.** Each blocking finding and its outcome (fixed, or accepted with
the decision heading) goes into the task's `## Progress`, so `flow-close` can see
patterns.

---

## 8. Fixing bugs

Every bug outside a hotfix, on the fast path or as a `bugfix` task:

1. **Reproduce.** Make it fail and record how (command, input, output). If you can't
   reproduce it, say what you tried; it is not ready to fix.
2. **Pin it with a failing check.** The smallest test or script that fails because of
   the bug: the task's `Check`, or the proof on the fast path.
3. **Name the root cause.** One or two sentences on why the code does the wrong thing,
   in `## Progress` (commit body on the fast path). Patching a symptom with the cause
   unknown is a hotfix (§4).
4. **Fix the cause, then look sideways.** Check every other caller and copy of the
   faulty rule; the sibling path is the usual second bug.
5. **Watch the check go green,** then run the real path once. Still red? Revert that
   fix and go back to step 3: a fix stacked on a wrong fix hides the cause. Two
   failed attempts on the same bug mean the cause is still unknown: stop, and make
   finding it a `spike` as below.

**The second of a kind buys a check.** When the same class of bug is fixed twice in
`maintenance` or on the fast path, add the check that would have caught it — a test,
a lint rule, a CI step — in that same change, instead of waiting for `flow-close`
(§5, which a small task never reaches).

Cause still unknown after reproducing? Finding it is a `spike` task ("what causes
<symptom>?", time box stated as what to stop after); the fix task is `Blocked by` it.
