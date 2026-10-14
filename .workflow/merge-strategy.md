# Merge and Branch Strategy

How finished work reaches the main branch, how a task is claimed, and when a
dependent task may start. `.workflow/standards.md` picks the landing mode with
`Landing:`; where the two disagree, standards.md wins.

The main branch is `main`, or `master` if that is what exists, unless
standards.md names another.

## Landing modes

**`direct`** — commit straight onto the current branch. No task branches. Suits
one person working in one tree; there is no parallel work to protect.

**`local-merge`** (default when `Landing:` is unset) — one branch per task,
`<effort>/<NN>-<slug>`, created from the main branch. After the task's commit,
rebase onto the main branch, rerun the tests, the task's Check and the real path
once. With one working tree, switch back to the main branch and fast-forward it.
If main is checked out in another worktree, fast-forward it there instead. Remove
any separate task worktree before deleting the task branch. Nothing is pushed, so
it suits one person, with parallel tasks in separate worktrees.

**`pr`** — one branch per task as above, pushed, merged through a pull request.
The task is landed when its PR is merged. Open the PR as the last step of
`flow-implement`. Use it whenever several people share a remote: pushed task
branches are how they see each other's claims.

## Work without a task file

Small tasks and hotfixes (CONVENTIONS §4) land by the same mode. Under `direct`,
commit on the current branch. Under `local-merge` and `pr`, branch from the main
branch as `small/<slug>` (a hotfix: `hotfix/<slug>`) and land it like a task
branch. There is nothing to archive. A hotfix still leaves its follow-up `bugfix`
task (CONVENTIONS §4).

## Claiming a task and where its state lives

Under `local-merge` and `pr`, the task branch is the claim, and the started task's
file (`Status`, `## Progress`, `## Blocked`) lives on that branch until it lands.
`.workflow/bin/workflow next` reads task branches, local and remote, and lists those
tasks as paused, needing replanning, in progress or finished-not-landed instead of
ready; under `pr`, `git fetch` first so teammates' branches show. A branch that
exists for a task you are not resuming means someone else has it. To resume a
paused or blocked task, check out its branch. Under `direct` there is one working
tree and the task file's `Status` is the whole story.

Under `pr`, fetch before claiming. Commit the task's start record and push its branch
before building so teammates can see the claim; do not replace an existing claim.

## Worktrees

Only under `local-merge` and `pr`, and only when two tasks run at the same time:
one working tree holds one branch, so parallel tasks need one worktree each.

```sh
git worktree add ../<repo>-<effort>-<NN> -b <effort>/<NN>-<slug> <main branch>
```

Name it after the task so a stray directory is traceable. `.workflow/` is a
relative path, so the new tree gets the task directory and the CLI with it; nothing
else needs copying. Land the task, then `git worktree remove <path>` before deleting
its branch. `git worktree prune` clears the records of trees deleted by hand. Run
`git worktree list` when you are unsure which tree is where.

Git refuses a branch that another worktree has checked out. That is the claim
(above) enforced by git rather than by convention: a second session cannot start a
task someone else holds. Under `direct` there is one tree and none of this applies.

## Dependent tasks

A task starts only when every blocker is archived under `.workflow/done/<effort>/`
and has landed on the main branch.

Under `pr` a dependent may stack instead of waiting: branch from the blocker's
branch and record `**Base branch:** <blocker branch>` in the task file. When the
blocker merges, rebase the dependent onto the main branch and drop the
`Base branch` line. Under `local-merge`, landing takes minutes, so wait.

## Base commit

`flow-implement` records `Base commit: <sha>` before the first edit. It is the
comparison point for `flow-verify`, never changes during implementation, and stays
in the task file as history. If the main branch moves ahead, don't rebase until
landing.

After a rebase the Base commit is still an ancestor, but `git diff <Base commit>`
now also contains everything that landed in between. A review rerun after a rebase
compares against `git merge-base <main branch> HEAD` instead.

## Merge style

Rebase and fast-forward (or rebase-merge on the hosting side): linear history,
easy to bisect. Squash-merge is the fallback. Task work gets no merge commits.

## Rebase and conflicts

1. `git fetch origin` (under `pr`)
2. `git rebase <main branch>` (or `origin/<main branch>`)
3. Resolve conflicts, `git rebase --continue`
4. Rerun the test suite, the task's Check, and the real path once
5. Under `pr`: `git push --force-with-lease`

The whole `flow-verify` doesn't rerun after a rebase: the change itself hasn't
moved. The Check and the real path do, because a rebase can break behaviour
without a textual conflict. A rebase, a landing or an integration merge starts a new
`Review boundary` record (CONVENTIONS §7): the comparison point becomes
`git merge-base <main branch> HEAD` and the round number goes up. If conflict
resolution changed the task's own lines, that resolution is a fix: CONVENTIONS §7
decides whether it goes back to the reviewer.

## Protection rules

Recommended for the main branch under `pr`: require PR review and status checks
(tests, lint); no force push, no deletion.
