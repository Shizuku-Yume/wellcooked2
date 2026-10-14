---
name: flow-spec
description: >-
  Turn settled conversation into a spec document. No interview. Use after design
  discussion or when user says "write the spec", "document the decision".
---

# Flow: Spec

Read `.workflow/CONVENTIONS.md` §2 (file structure) and `.workflow/STYLE.md` first.

Input: the conversation and design discussion. Output: `.workflow/specs/<effort>.md`, named by the effort slug its tasks will use.

Write from what was decided; the user isn't interviewed again. Decisions confirmed in `flow-grill` stay settled. Anything genuinely undecided goes under "Open questions" and you keep going.

## Two mistakes to avoid

**Writing implementation** — a spec that names every function is stale in a week. Name modules, boundaries, contracts, shapes.

**Writing only conclusions** — a spec without the rejected option is a press release. The next session reads it, disagrees with invisible reasoning, and reopens the decision.

## Before writing

1. **Collect the decisions already made** — from the conversation, `.workflow/decisions.md` and the user's message. Fill sections only with real decisions.
2. **Read the code** this will touch (CONVENTIONS §0).

## The document

```markdown
# <Title>

**Status:** proposed
**Priority:** normal

## What this is for
Problem from user's perspective. What's broken, missing, or wanted, and what
happens if nobody does this. Must stand alone.

## What it does
Behavior after the change, from user's perspective. Not implementation; what
someone notices is different. Concrete enough to demo.

## How it is built
Decisions that shape code. Modules added/changed, boundaries, data shapes,
interfaces, failure behavior. Prose and short lists; snippet only when clearer
(state machine, schema, type). Say what each piece is responsible for and what
it's not.

## What it must not do
Out of scope, explicitly, with reason. Things a reasonable reader might assume
are included. Prevents scope creep.

## What was considered and rejected
One entry per real alternative:
- **<The option>.** Its strongest argument, stated fairly. Then constraint that
  killed it.

## How we will know it works
Checks that prove behavior, each with an ID. Tasks list the IDs they make pass
in `Covers`; `flow-close` runs all of them against the finished effort.
- **W1.** <what command, what result>
- **W2.** ...

Also:
- What's tested at which level, and why that level.
- Observable signal when it breaks.
- Edges worth testing: empty input, big input, concurrent use, failure of each
  external dependency, permission denied.

If anything is "we will see", say so plainly.

## Open questions
Unresolved decisions, each with what would settle it and who can settle it.
Empty is good. Long list means spec not ready to build from.

## Notes
Anything a reader would otherwise ask. Links to task files, related specs.
```

`Priority` is optional (`critical | high | normal | low`, default `normal`); `next` ranks ready tasks by it. `Status` starts `proposed`; `flow-implement` and `flow-close` move it on (CONVENTIONS §6).

## Rules

- **Numbers, not adjectives** — "handles large imports" says nothing; "10k rows under 2s" is a spec. If nobody knows the number, say that.
- **Updated in place** — a reversed decision rewrites the spec (CONVENTIONS §2), so one document never holds contradicting answers. Once tasks exist, a spec change goes through `flow-break` amend so they follow it (CONVENTIONS §5).
- **Check IDs are stable** — a dropped check keeps its ID out of use; a new one takes the next free number.
- **Write for someone reading in six months.**
- **Name modules, not paths that will move.**
- **Delete sections with nothing to say** — a missing section says "didn't apply"; an empty one looks forgotten.

## Convergence gate

Before presenting, verify:

**Completeness:**
- "What this is for" stands alone as a problem statement
- "What it does" describes observable behavior
- "How it is built" names modules, boundaries, data shapes
- "How we will know it works" has runnable checks, each with an ID
- No duplicate facts across sections, no contradictions (the latest decision wins)
- Terms match `.workflow/glossary.md` or are newly added
- Decisions the spec relies on cite their entry as `decisions.md: <date> - <title>`

**Resolvability:**
- Every open question names who or what settles it
- No placeholder text ("TBD", "to be decided"), no unresolved alternatives disguised as notes

**Evidence:**
- The modules read are named
- Rejected alternatives have real reasons
- Numbers replace adjectives where they matter

Fix any failure before presenting.

## Finish

Write the file, then:

- **`flow-break` follows in this session (the normal case):** go straight to it. Its single review round covers the spec summary and the tasks; one approval settles both.
- **The session ends after the spec:** report the path, what the spec decides (one line), and open questions or "ready to build". Ask for explicit confirmation, then commit the spec with its decision and glossary entries per CONVENTIONS §2.

If the spec changes materially after approval, rerun the convergence gate and get fresh approval.

Building always goes through `flow-break`, even for one task: the task file carries the Base commit, the Check and the archive.
