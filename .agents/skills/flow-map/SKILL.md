---
name: flow-map
description: >-
  Plan work too big to see all the way through, as a map of open questions across
  sessions. Use when a project is large or foggy, or the user says "I don't know how
  to approach this".
---

# Flow: Map

Read `.workflow/CONVENTIONS.md` §1 (decision protocol) and §2 (file structure), and
`.workflow/STYLE.md`. Per CONVENTIONS §0, check the codebase before asking; many
apparent decisions are facts waiting to be found.

Use it when the plan can't be written yet because it depends on unanswered
questions: the work spans sessions, grilling keeps stalling on questions that hang
on others, or the user knows the goal but not the first step. If you can already list the tasks, use `flow-spec` and `flow-break`; a map for
clear work is ceremony.

Answer the gating questions in dependency order until the way is clear. Keep each
session focused, but answer independent questions together when their answers are clear.

## Map or spec

A **map** holds questions whose answers are decisions. A **spec** holds decisions
already made. A map graduates into one or more specs and is finished when it is
empty.

## Charting the map

### 1. Name the destination

Say in one or two sentences what the end looks like: a shipped feature, a locked
decision, a finished migration, a written document. The destination sets the scope,
so a wrong one makes every question wrong. If it is unclear, settle it with
`flow-grill`; it is the one question you can't defer.

### 2. Fan out and find questions

Grill the user **wide rather than deep**, across the whole space, for the questions
that gate everything else, not for answers. If no fog appears, the route is visible:
say so and use `flow-spec` and `flow-break`.

### 3. Write the map

Write `.workflow/maps/<slug>.md`:

```markdown
# Map: <destination in a few words>

**Destination:** <what reaching the end looks like>
**Started:** <date>   **Status:** charting | working | clear

## Route so far
<!-- One line per answer, newest last; detail lives in its question's section. -->

## Open questions
<!-- Answerable now: all dependencies settled. Grouped by what they block. -->

## Not yet clear
<!-- Coming, but hanging on answers we don't have, so not sharp yet. Written
loosely. This is fog. -->

## Ruled out
<!-- Beyond the destination: named, out of this effort, with a reason. Never
graduates. -->
```

Give each open question its own section below the header blocks:

```markdown
## Q<N>: <question, as a question>

**Blocks:** <what can't start until this is answered>
**Kind:** research | decide | build-a-bad-version | do-something-first
**Depends on:** <Q numbers, or none>
**Answer:** <filled in when answered>
```

Route so far only points; detail lives in each question's section. A map that
states its answers twice drifts.

Commit the new map (and any decision entries) when the charting session ends, per
CONVENTIONS §2.

### 4. Four kinds

Each kind says how to answer the question.

- **research**: a findable fact: docs, a third-party API, a library.
  Answer it with a subagent, in parallel, without the user.
- **decide**: a real choice between options. Answer it with `flow-grill`.
- **build-a-bad-version**: how something should look or behave, which talking can't
  settle. Build the cheapest rough thing to
  react to (stub, sketch, throwaway page); keep or discard it after.
- **do-something-first**: nothing to decide, but discussion needs something to exist
  first (an API account, moved data, access). Do it; the answer records what was
  done and what came out.

Fill in `Depends on` once all questions exist, so the numbers are real. A question
with every dependency answered is **open**; the rest are blocked or fog.

## Where lines go

**Question or fog?** Sharpness decides, not answerability. Anything you can state
precisely now is a question, even if it is blocked; the rest is fog. Keep fog
coarse: one patch may later become three questions, or none.

**Fog or ruled out?** Fog is in scope but unclear; ruled-out work lies beyond the
destination. Ruled-out work never graduates. If the destination changes, it returns
as a new effort, not a resumed one.

## Working the map

Answer the first open question, or the one the user named, and keep the session focused.
Answer more than one when the answers are already clear and independent.

1. Read the map header and Route so far, not every question section.
2. Pick the first open question, or the one the user named.
3. Answer it as its kind says, reading related question sections as needed.
4. In the question's section, record the answer and what it was based on. Add a
   decision entry only when CONVENTIONS §1.2 calls for one. The first answer moves
   the map from `Status: charting` to `Status: working` (CONVENTIONS §6).
5. Add the answer to Route so far, answer any question it just unblocked, and turn
   fog that is now sharp into questions, removing it from "Not yet clear".
6. Move anything the answer shows beyond the destination to "Ruled out" with a
   reason, instead of resolving it on the route.
7. Commit the map and any decision entries at the end of the session, per
   CONVENTIONS §2.

## Finishing

When no open questions or promotable fog remain, the route is clear. Write the specs
it implies (`flow-spec`), cut them into tasks (`flow-break`), and mark the map
`Status: clear` with a closing line saying what it produced.

Keep the map: it records why the plan looks as it does, which someone will ask in
three months.
