<!-- workflow:start -->
# Project workflow

Before non-trivial work, read `.workflow/standards.md` (this project's commands, layout
and landing) and `.workflow/STYLE.md` (how to write). Read only the `.workflow/CONVENTIONS.md`
sections named by the skill you are using; do not preload the whole rulebook. The steps are
skills in `.agents/skills/`; when the next step isn't obvious, start with `flow-start`.

When rules disagree: the user's current instruction, then `.workflow/standards.md` and
the repository's existing conventions (commit style in `git log`, lint and formatter
config, CONTRIBUTING), then CONVENTIONS.md, then STYLE.md.

Always:
1. Find facts yourself; ask the user only what they own, in batches (CONVENTIONS §0, §1).
2. Run it and observe the result before saying it is done.
3. A plan that turns out wrong is fixed in the plan (CONVENTIONS §5).
4. One task at a time per working tree; parallel tasks get separate worktrees.
<!-- workflow:end -->
