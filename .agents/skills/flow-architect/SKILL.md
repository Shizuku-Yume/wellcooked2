---
name: flow-architect
description: >-
  Find code that makes the next change expensive: duplication, dead weight, leaky
  modules, hand-rolled basics. Use when the user says "audit the codebase" or "find duplication".
---

# Flow: Architect

Read `.workflow/STYLE.md`: everything you report must read like a person wrote it.
Per CONVENTIONS §0, check the codebase before asking.

Find the code that makes the next change expensive and say what to do. Report
only; change nothing. Aim for a short report someone acts on, and prefer deleting
three files to a clever redesign.

## Look at code that changes

Maintenance cost follows change, not size. A sprawling module nobody touches isn't
worth fixing today.

Use the area the user named. Otherwise find the files recent history keeps hitting:

```sh
git log --oneline -60
git log --format= --name-only -60 | sort | uniq -c | sort -rn | head -30
```

Start with those hot spots. If the history is scattered, say so and widen the scan.

Read `.workflow/glossary.md` and the area's specs first: some of what looks wrong
may be a recorded decision.

## What to hunt

**Duplication**: the same logic written twice; the second copy is where the bug
hides. Report the shared shape and where it should live. The same *rule* written
differently matters more than identical lines.

**Dead weight**: unused exports, flags nobody sets, config for a value that never
changes, an interface with one implementation, a factory for one product, a wrapper
that only delegates, a compatibility path for a caller that no longer exists. Grep
for callers before reporting; "dead" code that is live makes a bad report.

**Hand-rolled basics**: something the standard library ships (say which function)
or the platform already does (`<input type="date">` instead of a date-picker
dependency, a CSS rule instead of JS, a database constraint instead of application
checks). Name the replacement.

**Unneeded dependencies**: a package pulled in for one format call, one parse or
one small utility. Name the standard-library or native replacement.

**Modules that leak**: raw data structures every caller must understand, arguments
that expose a function's internals, two modules reaching into each other. Say what
the boundary should be.

**Modules that are just plumbing**: a layer that only forwards calls, or an
interface as complicated as its body. Ask: if I deleted it, would the complexity
concentrate somewhere better, or just move? If it just moves, leave it.

**Logic that can't be tested where it sits**: real behaviour inside functions that
need a database, network or UI to run at all. Say what would let it be checked.

Open each candidate with the concrete symptom: the file, the name, what it does
wrong. "Module not cohesive" is not a symptom.

## Rank by what it costs to leave alone

Put the biggest pain first: how often the code is touched, how many changes it
forces, how hard it is to understand the first time. An ugly file nobody edits ranks
below a tidy file every feature must modify.

Use three labels: **Fix now**, **Worth doing**, **Only if it grows**.

## Report format

Write Markdown, in the conversation or in a file if long. Skip HTML and decorative
diagrams; a short before/after sketch is fine where structure is the point.

```markdown
# Architecture review: <area> (<date>)

## Where time goes
<two or three lines: where recent changes keep landing>

## Findings

### 1. <Concrete symptom> [Fix now]

**Where:** <files>
**What is wrong:** <specific problem with evidence: rule in three places; flag set nowhere; two modules import each other>
**What to do:** <the change, in plain terms>
**What it buys:** <what gets easier: fewer places to edit, test that can exist, dependency that goes away>
**Cost:** <what the change risks, how big it is>

### 2. ...

## What I would not touch
<candidates that look bad but aren't worth it, one line each with the reason, so
the next audit doesn't re-propose them>
```

End with two lines: the finding count and the one you'd do first.

## Rules

- **Count deletion**: for anything you propose removing, state the size change:
  `-N lines, -M dependencies`. A proposal that neither shrinks code nor makes a
  specific change easier is a preference, not a finding.
- **Never propose a rewrite**: each proposal must fit in a few tasks, or break into
  tasks that do.
- **Respect recorded decisions**: raise a finding against a spec or logged decision
  only when the code has since made it actively painful, and say so: "contradicts
  decision in <spec>, but costs us X every time we touch this".
- **Correctness bugs found along the way** get one line under a separate heading,
  then drop them. This skill is about structure, not a bug hunt.
- **Stop when the money is on the table**: five sharp findings beat twenty padded
  ones. If you find only two, report two and say the area is in good shape.

## After the report

Nothing has changed. The user picks what to act on; each chosen finding becomes its
own effort (`flow-grill`, then `flow-spec` and `flow-break`). Findings that pass as
small tasks (CONVENTIONS §4) can just be fixed.

**Track unaddressed findings**: append one entry per finding to `## Entries` in
`.workflow/technical-debt.md`, in the format that file shows. The heading is the
concrete symptom; `Priority` maps from the label: Fix now → `fix-now`, Worth doing →
`worth-doing`, Only if it grows → `only-if-grows`. This stops future reviews
re-discovering them and keeps the reason each wasn't fixed.

**Findings the user declines**: delete the finding's entry from `technical-debt.md`
if it has one, and append a decision entry to `.workflow/decisions.md` (Decided: not
fixing <symptom>; Instead of: fixing it; Because; Mine: no; Revisit when), so the
next review doesn't re-propose it.
