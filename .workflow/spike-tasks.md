# Spike Tasks

Exploratory work where the output is an answer and the code may be throwaway.

## What is a Spike?

A spike is a task where:
- Goal is learning, not shipping
- Solution approach is unknown
- The answer is the deliverable, and the code that produced it may be discarded

## When to Use Spikes

**Good reasons:**
- "Can this library do X?"
- "How does this API actually work?"
- "Is this architecture viable?"
- "What's the performance of approach A vs B?"
- "What causes this bug?" when it reproduces but nobody can say why
  (CONVENTIONS §8); the fix task is `Blocked by` the spike

**Not a spike:**
- Feature is hard → build it properly
- Requirements unclear → use `flow-grill`
- Skipping the plan → plan it

## Spike Task Format

Same file format as any task (CONVENTIONS §2.1), plus:

```markdown
**Task type:** spike
**Question:** <what needs answering>
**Time box:** <what to stop after: one prototype, one benchmark run, N approaches tried>
```

**The three things that differ from a regular task:**
- `Delivers` is "Answer to: <question>"; the Check is "question answered with
  evidence", not "feature works".
- Time box is a scope limit, not wall-clock hours: an agent can't measure hours, but
  it can tell when it has built the one prototype it was allowed. Stop at the limit
  even if the answer is incomplete, and report what's still unknown.
- No production code is required, and code may be thrown away (see below).

## After a Spike

Write findings into the task file's `## Findings` section, then record them where they'll be read:
- Spec (if spike validates approach)
- Decision log (if spike rules out option)
- Glossary (if spike clarifies concept)
- New task (if spike shows what to build)

**Spike code disposal** (when `flow-implement` finishes the spike):
- If keeping: clean it up, add tests, make it real; that's a follow-up task, not the spike
- If discarding: drop it before committing, or leave it on a `spike/<effort>-<NN>` branch named in Findings
- Never leave spike code in main

The task file itself is archived to `.workflow/done/<effort>/` like any task.

## Example Spike Task

```markdown
# 03: Evaluate GraphQL federation performance

**Effort:** api-v2
**Task type:** spike
**Base commit:** <commit-sha>

**Question:** Can federated GraphQL handle 1000 req/s with acceptable latency?
**Time box:** one gateway with two subgraphs, one k6 run; no tuning

**Delivers:** Answer to: is federation viable at 1000 req/s?

**Blocked by:** None
**Files:** throwaway gateway on a spike branch

**Read first:** API v2 spec, "Performance" section

**Check:** `k6 run load.js` → p95 latency recorded in Findings

- [ ] Load test results documented in Findings
- [ ] Decision recorded: use federation or not

## Findings

<Filled in after spike>

**Result:** Federation adds 12ms overhead at p95, well within budget
**Recommendation:** Proceed with federation
**Recorded in:** decisions.md: 2026-02-10 - Use GraphQL federation for API v2
```
