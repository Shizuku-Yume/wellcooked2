# Output Style

How to write anything a person reads: chat replies, documents, commit messages,
UI text, code comments, in every response, workflow task or not. It ranks last:
the user's instruction, `.workflow/standards.md` and the repository's existing
conventions win over it (see **Precedence** at the top of `CONVENTIONS.md`).

## Language

Write in the language the user wrote in, and keep each document in one language.
Code, identifiers, commands, file paths and API names keep their original form
inside either. Field names in structured formats (such as the decisions.md
headers) stay in English for tool compatibility; their content follows the
user's language.

## Chat

Lead with the answer and stop when it is complete; the reader can already see the
question and what came before. Use the plain words a competent colleague would say
out loud.

## Terms

Use the project's own vocabulary: if the project calls it an "issue", so do you.
Define a technical term in the sentence where it first appears. Call a thing by
its established name.

## Front-end text

Text on a page a person uses is written for that person:

- Say what the thing is and what happens when they press it.
- Use words the user knows; implementation names, framework terms and error codes
  belong in the code. "Sync failed", not "HTTP 409 from the upstream provider".
- When something breaks, say what happened to their data and what they can do next.
- Give empty states, tooltips and buttons real sentences.

Technical detail goes in a code comment next to the relevant line.

## Code comments

Comment what the code cannot say itself: why this order, why this guard, what
breaks if it changes. Let the code show what it does.

## Commit messages

Follow the repository's existing convention, as seen in `git log --oneline -10`.
When it has none, write one imperative line saying what changed and why. Details
go in the body.

## Length and reporting

Match length to the question: a yes/no question gets the answer plus one line of
why; a design question gets the design.

A work report says what changed, what was verified, and what was not done.
