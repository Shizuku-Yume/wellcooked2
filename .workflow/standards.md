# Standards

Fill this in once, at the start of the project. Keep it under one screen: it is
read at the start of every session, so every extra line costs attention that the
real work needs.

Delete the lines that do not apply. Replace every `<...>`. Where this file
disagrees with CONVENTIONS.md or STYLE.md, this file wins.

## What this project is

<One or two sentences. What it does, who uses it. Enough that someone can tell
whether a change belongs here.>

## Running it

```sh
<install command>      # e.g. npm install, uv sync, cargo build
<run command>          # e.g. npm run dev
<test command>         # e.g. pytest, npm test
<typecheck command>    # e.g. tsc --noEmit
<lint command>         # e.g. ruff check .
```

Run the test command before claiming anything works.

## Layout

<Where the parts live, one line each. Keep it short: `src/api` - HTTP handlers.
Not a file tree.>

Workflow files live where CONVENTIONS §2 says.

## How code is written here

<The rules that a reviewer would enforce and that are not obvious from reading
three files. Two or three lines each, no more. For example: errors are returned,
never thrown; every database call goes through the repository layer; tests are
named after the behaviour, not the function.>

If a rule is long enough to need a section, it belongs in its own document under
`.workflow/`, linked from here.

## What we do not do

<The approaches this project has ruled out, with the reason. This is the section
that stops the same argument from being had again.>

## Branches and landing

Landing: <direct | local-merge | pr>

How finished work reaches the main branch: `direct` commits on the current branch,
`local-merge` (the default) lands a branch locally, `pr` through a pull request.
Several people sharing a remote: use `pr`.
`.workflow/merge-strategy.md` describes each. Name the main branch here if it is
neither `main` nor `master`.
