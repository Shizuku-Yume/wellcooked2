#!/usr/bin/env python3
"""workflow-core.py - `validate` and `next` for the workflow toolkit.

Called by bin/workflow as `python3 workflow-core.py validate|next ...`.
Python 3.8+, standard library only. Reads task files under .workflow/tasks/
and .workflow/done/, specs under .workflow/specs/, and .workflow/decisions.md.

Metadata grammar (CONVENTIONS §2): fields are `Name: value` lines, plain,
bulleted (`- Name: value`) or bolded (`**Name:** value`); fenced code blocks
and HTML comments are ignored; indented or bulleted lines continue the
previous field (`Blocked by` joins with ", ", other fields with a space);
blank lines, headings, checklists and new fields end a continuation.
`## Goal` and `## Check` headings start sections whose lines join the same
way. The first occurrence of a field wins; later ones are duplicates.
"""

import json
import os
import re
import subprocess
import sys

VERSION = "2.6.4"

KNOWN_FIELDS = {
    "task", "effort", "task type", "base commit", "base branch", "delivers", "blocked by",
    "files", "read first", "check", "covers", "done when", "priority", "status",
    "started", "finished", "question", "time box", "goal", "scope", "spec",
    "dependencies", "notes",
}
FIELD_ALIASES = {"base": "base commit", "type": "task type"}
TASK_TYPES = ("feature", "bugfix", "spike", "refactor")
PRIORITY_RANK = {"critical": 0, "urgent": 0, "high": 1, "normal": 2, "low": 3}
SPEC_STATUSES = ("proposed", "in progress", "done")
TASK_STATUSES = ("active", "paused", "blocked")
FINISHED_STATUSES = ("done", "completed", "archived")
DECISION_FIELDS = ("Decided", "Instead of", "Because", "Mine", "Revisit when")

SLUG_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
TASK_FILE_RE = re.compile(r"^([0-9]+)-.+\.md$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
TITLE_RE = re.compile(r"^#\s+([0-9]+):\s*(.*)$")
H2_RE = re.compile(r"^##\s+(.*)$")
CHECKLIST_RE = re.compile(r"^[-*]\s+\[[\sxX]\]")
BULLET_RE = re.compile(r"^[-*]\s+(.*)$")
BOLD_FIELD_RE = re.compile(r"^\*\*([^*]+)\*\*\s*:?\s*(.*)$")
PLAIN_FIELD_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_. -]*):\s*(.*)$")
NONE_RE = re.compile(r"^none(\s*,\s*can\s+start\s+now)?$")
QUALIFIED_REF_RE = re.compile(r"^([a-z0-9]+(-[a-z0-9]+)*)/([0-9]+)$")
BARE_REF_RE = re.compile(r"^([0-9]+)$")
SPEC_CHECK_RE = re.compile(r"^[-*]\s+\*\*([A-Za-z][A-Za-z0-9_-]*[0-9])\.\*\*")


# ---------------------------------------------------------------- helpers

def project_root():
    try:
        out = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             universal_newlines=True, check=False)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except OSError:
        pass
    return os.getcwd()


def read_lines(path):
    """Lines of a file with the newline and one trailing CR removed."""
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        text = fh.read()
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return [line[:-1] if line.endswith("\r") else line for line in lines]


def norm_number(value):
    stripped = value.lstrip("0")
    return stripped if stripped else "0"


def task_ref(effort, number):
    return "%s/%02d" % (effort, int(number))


def task_files(directory):
    """Every Markdown file under a task directory, at any depth, sorted."""
    found = []
    if not os.path.isdir(directory):
        return found
    for base, dirs, files in os.walk(directory, followlinks=False):
        for name in files:
            if name.endswith(".md"):
                found.append(os.path.join(base, name))
        for name in dirs:
            full = os.path.join(base, name)
            if os.path.islink(full) and name.endswith(".md") and os.path.isfile(full):
                found.append(full)
    return sorted(found, key=lambda p: p.encode("utf-8", "surrogateescape"))


def dir_effort(base, path):
    rel = os.path.relpath(path, base).replace(os.sep, "/")
    return rel.split("/", 1)[0] if "/" in rel else ""


def strip_comments(line, in_comment):
    """Remove HTML comment text; returns (line, in_comment)."""
    while True:
        if in_comment:
            if "-->" in line:
                line = line.split("-->", 1)[1]
                in_comment = False
            else:
                return "", True
        elif "<!--" in line:
            before, after = line.split("<!--", 1)
            if "-->" in after:
                line = before + after.split("-->", 1)[1]
            else:
                return before, True
        else:
            return line, False


class Meta(object):
    """Fields of one task or spec file."""

    def __init__(self):
        self.field = {}
        self.line = {}
        self.dups = []          # (line, name)
        self.title = ""
        self.title_number = ""

    def get(self, name, default=""):
        return self.field.get(name, default)


def read_fields(path, lines=None):
    meta = Meta()
    if lines is None:
        lines = read_lines(path)
    in_fence = in_comment = False
    current = kind = None
    section_content = False
    title_seen = False
    sections = {}
    for lineno, line in enumerate(lines, 1):
        line, in_comment = strip_comments(line, in_comment)
        if FENCE_RE.match(line):
            in_fence = not in_fence
            current = kind = None
            continue
        if in_fence:
            continue
        rawline = line
        line = line.strip()
        if not line:
            if kind and (kind == "field" or section_content):
                current = kind = None
            continue
        m = TITLE_RE.match(line)
        if m:
            if not title_seen:
                title_seen = True
                meta.title = m.group(2).strip()
                meta.title_number = norm_number(m.group(1))
            current = kind = None
            continue
        m = H2_RE.match(line)
        if m:
            heading = m.group(1).lower()
            if heading.endswith(":"):
                heading = heading[:-1]
            heading = heading.strip()
            current = kind = None
            if heading in ("goal", "check"):
                current, kind, section_content = heading, "section", False
            continue
        if line.startswith("#") or CHECKLIST_RE.match(line):
            current = kind = None
            continue
        clean = line
        m = BULLET_RE.match(clean)
        if m:
            clean = m.group(1)
        m = BOLD_FIELD_RE.match(clean) or PLAIN_FIELD_RE.match(clean)
        if m:
            key = m.group(1).lower()
            if key.endswith(":"):
                key = key[:-1]
            key = FIELD_ALIASES.get(key.strip(), key.strip())
            if key in KNOWN_FIELDS:
                if key not in meta.line:
                    meta.field[key] = m.group(2).strip()
                    meta.line[key] = lineno
                    current, kind = key, "field"
                else:
                    meta.dups.append((lineno, key))
                    current, kind = None, "dup"
                continue
            current = kind = None
            continue
        if kind == "section":
            if current not in meta.line:
                old = meta.field.get(current, "")
                meta.field[current] = (old + " " + line) if old else line
            else:
                old = sections.get(current, "")
                sections[current] = (old + " " + line) if old else line
            section_content = True
            continue
        if kind in ("field", "dup") and (rawline[:1].isspace() or BULLET_RE.match(line)):
            if kind == "field":
                old = meta.field.get(current, "")
                sep = ", " if current == "blocked by" else " "
                meta.field[current] = (old + sep + clean) if old else clean
            continue
        current = kind = None
    if not meta.field.get("check") and sections.get("check"):
        meta.field["check"] = sections["check"]
    return meta


class BlockerError(Exception):
    def __init__(self, token, trailing=False):
        Exception.__init__(self, token)
        self.token = token
        self.trailing = trailing


def parse_blockers(raw):
    """[(normalized ref, raw token)] for a Blocked by value; raises BlockerError."""
    raw = raw.strip()
    if NONE_RE.match(raw.lower()):
        return []
    if not raw:
        raise BlockerError("")
    if raw.endswith(","):
        raise BlockerError("", trailing=True)
    refs = []
    for token in raw.split(","):
        token = token.strip()
        m = QUALIFIED_REF_RE.match(token)
        if m:
            refs.append((m.group(1) + "/" + norm_number(m.group(3)), token))
            continue
        m = BARE_REF_RE.match(token)
        if m:
            refs.append((norm_number(m.group(1)), token))
            continue
        raise BlockerError(token)
    return refs


def resolve_ref(ref, effort):
    if "/" in ref:
        ref_effort, number = ref.split("/", 1)
        return task_ref(ref_effort, number)
    return task_ref(effort, ref)


def colors_enabled(no_color_flag):
    return (not no_color_flag and not os.environ.get("NO_COLOR")
            and sys.stdout.isatty())


def usage_error(message, usage):
    sys.stderr.write("Error: %s\n" % message)
    if usage:
        sys.stderr.write(usage)
    return 2


# Options dropped in 2.5, answered like the removed commands in bin/workflow.
REMOVED_FLAGS = {
    "--fix": "validate --fix was removed in 2.5: CRLF line endings are read like LF, so there is nothing to repair",
    "--interactive": "next --interactive was removed in 2.5: run 'workflow next' and start a task with flow-implement <effort>/<NN>",
}


def parse_common(args, usage, allowed):
    """Parse --format/--no-color/--strict; returns (options, exit code or None)."""
    opts = {"format": "text", "no_color": False, "strict": False}
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in ("-h", "--help"):
            sys.stdout.write(usage)
            return opts, 0
        if arg == "--no-color":
            opts["no_color"] = True
        elif arg == "--strict" and "strict" in allowed:
            opts["strict"] = True
        elif arg == "--format":
            if i + 1 >= len(args):
                return opts, usage_error("--format requires text or json", "")
            opts["format"] = args[i + 1]
            i += 1
        elif arg.startswith("--format="):
            opts["format"] = arg[len("--format="):]
        elif arg in REMOVED_FLAGS:
            return opts, usage_error(REMOVED_FLAGS[arg], "")
        else:
            return opts, usage_error("unknown option: %s" % arg, usage)
        i += 1
    if opts["format"] not in ("text", "json"):
        return opts, usage_error("invalid format: %s (use text or json)" % opts["format"], "")
    return opts, None


# --------------------------------------------------------------- validate

VALIDATE_USAGE = """workflow validate - validate task files and the decision log

Usage: workflow validate [--strict] [--format text|json] [--no-color]
Checks required Task, Effort, Check and Blocked by fields; effort slugs;
that a task in tasks/<effort>/ says the same Effort; that the heading number
matches the filename; Status and Priority values; blockers; Base commit refs;
task-number collisions within efforts; dependency cycles; Covers IDs no spec
check defines; and decision format. Checks active and completed task files;
problems in completed files warn instead of failing.

  --strict     Treat warnings as errors
  --format F   Output text (default) or JSON
  --no-color   Disable color output
  --help       Show help

Exit: 0 = valid, 1 = warnings, 2 = errors (including usage errors).
"""


class Validator(object):
    def __init__(self, root):
        self.root = root
        self.wf = os.path.join(root, ".workflow")
        self.issues = []
        self.errors = self.warnings = 0
        self.files = []
        self.meta = {}
        self.number = {}
        self.effort = {}
        self.dir_effort = {}
        self.index = {}
        self.active = set()
        self.done_dir = os.path.join(self.wf, "done") + os.sep
        self.lines_cache = {}

    def rel(self, path):
        rel = os.path.relpath(path, self.root)
        return rel.replace(os.sep, "/")

    def issue(self, severity, path, line, message, suggestion):
        if severity == "ERROR":
            self.errors += 1
        else:
            self.warnings += 1
        self.issues.append({"severity": severity, "file": self.rel(path),
                            "line": int(line or 1), "message": message,
                            "suggestion": suggestion})

    def sev(self, path):
        return "WARNING" if path.startswith(self.done_dir) else "ERROR"

    def line_of(self, path, key):
        return self.meta[path].line.get(key, 1)

    def run(self):
        if not os.path.isdir(self.wf):
            self.issue("ERROR", self.wf, 1, "missing .workflow directory", "Run: workflow init")
            return
        if not os.path.isdir(os.path.join(self.wf, "tasks")):
            self.issue("WARNING", os.path.join(self.wf, "tasks"), 1,
                       "tasks directory is missing", "Run: workflow init")
        self.collect()
        self.check_required()
        self.check_types_and_slugs()
        self.check_layout()
        self.check_collisions()
        edges = self.check_blockers()
        self.check_status()
        self.check_priorities()
        self.check_covers()
        self.check_bases()
        self.check_cycles(edges)
        self.check_decisions()

    def collect(self):
        for sub in ("tasks", "done"):
            directory = os.path.join(self.wf, sub)
            for path in task_files(directory):
                m = TASK_FILE_RE.match(os.path.basename(path))
                if not m:
                    continue
                self.files.append(path)
                self.number[path] = norm_number(m.group(1))
                self.dir_effort[path] = dir_effort(directory, path)
                meta = read_fields(path)
                self.meta[path] = meta
                for line, name in meta.dups:
                    self.issue(self.sev(path), path, line, "duplicate %s field" % name,
                               "Keep one %s field." % name)
                self.effort[path] = meta.get("effort")
                key = task_ref(self.effort[path], self.number[path])
                self.index.setdefault(key, path)
                if sub == "tasks":
                    self.active.add(key)

    def check_required(self):
        for path in self.files:
            meta = self.meta[path]
            for name in ("Task", "Effort", "Check", "Blocked by"):
                if name == "Task":
                    value = meta.title or meta.get("task")
                    line = 1 if meta.title else meta.line.get("task", 1)
                else:
                    key = name.lower()
                    value = meta.get(key)
                    line = meta.line.get(key, 1)
                if not value:
                    self.issue(self.sev(path), path, line,
                               "missing or empty required %s field" % name,
                               "Add a nonempty %s field (Task may be '# NN: title'; "
                               "Check may be a section)." % name)

    def check_types_and_slugs(self):
        for path in self.files:
            value = self.meta[path].get("task type")
            if value and value not in TASK_TYPES:
                self.issue(self.sev(path), path, self.line_of(path, "task type"),
                           "invalid Task type '%s'" % value,
                           "Use feature, bugfix, spike, or refactor.")
            effort = self.effort[path]
            if effort and not SLUG_RE.match(effort):
                self.issue(self.sev(path), path, self.line_of(path, "effort"),
                           "invalid effort slug '%s'" % effort,
                           "Use lowercase letters, digits and hyphens.")

    def check_layout(self):
        for path in self.files:
            directory, effort = self.dir_effort[path], self.effort[path]
            if directory and effort and directory != effort:
                self.issue(self.sev(path), path, self.line_of(path, "effort"),
                           "task is under %s/ but its Effort is '%s'" % (directory, effort),
                           "Move it to the %s/ directory or correct the Effort field." % effort)
            title_number = self.meta[path].title_number
            if title_number and title_number != self.number[path]:
                self.issue(self.sev(path), path, 1,
                           "heading number %s does not match filename number %s"
                           % (title_number, self.number[path]),
                           "Make the '# NN:' heading and the filename use the same number.")

    def check_collisions(self):
        seen = {}
        for path in self.files:
            key = task_ref(self.effort[path], self.number[path])
            if key in seen:
                self.issue("ERROR", path, 1,
                           "task-number collision: %s also occurs in %s" % (key, self.rel(seen[key])),
                           "Renumber one task and update its blocker references.")
            else:
                seen[key] = path

    def check_blockers(self):
        edges = {}
        suggestion = "Use NN or effort/NN, comma-separated, or None."
        for path in self.files:
            blocked = self.meta[path].get("blocked by")
            if not blocked:
                continue
            line = self.line_of(path, "blocked by")
            key = task_ref(self.effort[path], self.number[path])
            try:
                refs = parse_blockers(blocked)
            except BlockerError as exc:
                if exc.trailing:
                    self.issue(self.sev(path), path, line, "malformed blocker: trailing comma", suggestion)
                else:
                    self.issue(self.sev(path), path, line, "malformed blocker '%s'" % exc.token, suggestion)
                continue
            for ref, raw in refs:
                target = resolve_ref(ref, self.effort[path])
                if target not in self.index:
                    self.issue(self.sev(path), path, line, "orphaned blocker '%s'" % raw,
                               "Create task %s or correct the Blocked by reference." % target)
                elif key in self.active and target in self.active:
                    edges.setdefault(key, []).append(target)
        return edges

    def check_covers(self):
        """Covers must name check IDs the effort's spec defines.

        Silent when the effort has no spec or the spec lists no IDs, so a project
        that writes its checks differently is never nagged.
        """
        cache = {}
        for path in self.files:
            covers = self.meta[path].get("covers")
            effort = self.effort[path]
            if not covers or not effort:
                continue
            if effort not in cache:
                cache[effort] = spec_check_ids(self.wf, effort)
            known = cache[effort]
            if not known:
                continue
            line = self.line_of(path, "covers")
            for token in covers.split(","):
                token = token.strip().strip(".")
                if token and token not in known:
                    self.issue(self.sev(path), path, line,
                               "Covers names '%s', which specs/%s.md does not define"
                               % (token, effort),
                               "Use one of: %s, or add the check to the spec."
                               % ", ".join(sorted(known)))


    def check_status(self):
        """A Status value decides whether `next` offers a task as ready.

        An unrecognized one is worse than a missing field: the task is silently
        treated as not started, so a half-finished task gets handed out again.
        """
        for path in self.files:
            value = self.meta[path].get("status").lower()
            if not value:
                continue
            line = self.line_of(path, "status")
            if value in TASK_STATUSES:
                continue
            if value in FINISHED_STATUSES:
                self.issue(self.sev(path), path, line,
                           "Status '%s' on a task file; only flow-implement archives a task"
                           % value,
                           "Move it to done/<effort>/ and drop the Status field.")
            else:
                self.issue(self.sev(path), path, line,
                           "invalid Status '%s'" % value,
                           "Use one of: %s (or drop the field for a drafted task)."
                           % ", ".join(TASK_STATUSES))

    def check_priorities(self):
        """The spec header's Priority is what `next` ranks by, and Status tracks lifecycle.

        A typo in Priority silently falls back to normal, so the effort loses its place
        in the queue without a word. A task-level Priority does nothing at all.
        """
        specs = {}
        for path in self.files:
            value = self.meta[path].get("priority")
            if value:
                self.issue(self.sev(path), path, self.line_of(path, "priority"),
                           "Priority on a task file, which nothing ranks by",
                           "Set Priority in specs/%s.md instead."
                           % (self.effort[path] or "<effort>"))
            effort = self.effort[path]
            if effort and effort not in specs:
                specs[effort] = True
        specs_dir = os.path.join(self.wf, "specs")
        if os.path.isdir(specs_dir):
            for name in sorted(os.listdir(specs_dir)):
                if name.endswith(".md"):
                    specs[name[:-3]] = True
        for effort in sorted(specs):
            path = os.path.join(self.wf, "specs", effort + ".md")
            if not os.path.isfile(path):
                continue
            meta = read_fields(path)
            value = meta.get("priority", "").lower()
            if value and value not in PRIORITY_RANK:
                self.issue("WARNING", path, meta.line.get("priority", 1),
                           "invalid Priority '%s'" % value,
                           "Use one of: %s. An unknown value ranks as normal."
                           % ", ".join(sorted(PRIORITY_RANK)))
            status = meta.get("status", "").lower()
            if status and status not in SPEC_STATUSES:
                self.issue("WARNING", path, meta.line.get("status", 1),
                           "invalid spec Status '%s'" % status,
                           "Use one of: %s." % ", ".join(SPEC_STATUSES))

    def check_bases(self):
        commits, branches = {}, {}
        for path in self.files:
            value = self.meta[path].get("base commit").strip()
            if value and value not in ("<commit-sha>", "not-started"):
                commits.setdefault(value, []).append(path)
            branch = self.meta[path].get("base branch").strip()
            if branch and not (branch.startswith("<") and branch.endswith(">")):
                branches.setdefault(branch, []).append(path)
        if not commits and not branches:
            return
        is_git = subprocess.run(["git", "-C", self.root, "rev-parse", "--git-dir"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        for value in sorted(commits):
            ok = False
            if is_git:
                ok = subprocess.run(["git", "-C", self.root, "rev-parse", "--verify", "--quiet",
                                     "--end-of-options", value + "^{commit}"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
            if not ok:
                for path in commits[value]:
                    self.issue(self.sev(path), path, self.line_of(path, "base commit"),
                               "invalid Base commit '%s'" % value,
                               "Use an existing git commit ref, or <commit-sha> for drafts.")
        for branch in sorted(branches):
            ok = False
            if is_git:
                for candidate in (branch, "origin/" + branch):
                    if subprocess.run(["git", "-C", self.root, "rev-parse", "--verify", "--quiet",
                                       "--end-of-options", candidate + "^{commit}"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                        ok = True
                        break
            if not ok:
                for path in branches[branch]:
                    self.issue(self.sev(path), path, self.line_of(path, "base branch"),
                               "invalid Base branch '%s'" % branch,
                               "Use an existing blocker branch name, or drop Base branch once rebased.")
    def check_cycles(self, edges):
        state = {}

        def visit(node, path):
            state[node] = 1
            for target in edges.get(node, ()):
                status = state.get(target, 0)
                if status == 1:
                    task = self.index[node]
                    self.issue("ERROR", task, self.line_of(task, "blocked by"),
                               "circular dependency: %s -> %s" % (" -> ".join(path), target),
                               "Remove or change one blocker in the cycle.")
                elif status == 0:
                    visit(target, path + [target])
            state[node] = 2

        sys.setrecursionlimit(max(1000, len(self.active) * 4 + 100))
        for node in sorted(self.active):
            if state.get(node, 0) == 0:
                visit(node, [node])

    def check_decisions(self):
        path = os.path.join(self.wf, "decisions.md")
        if not os.path.isfile(path):
            self.issue("WARNING", path, 1, "decision log is missing",
                       "Create .workflow/decisions.md with ## Entries.")
            return
        for line, message, suggestion in decision_problems(read_lines(path)):
            self.issue("ERROR", path, line, message, suggestion)



ISO_TIME_RE = re.compile(r"^[0-9-]+T[0-9][0-9]:[0-9][0-9](:[0-9][0-9](\.[0-9]+)?)?"
                         r"(Z|[+-][0-9][0-9]:[0-9][0-9])?$")
DATE_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
DECISION_HEADING_RE = re.compile(r"^[^ \t]+[ \t]+-[ \t]+[^ \t]")
DECISION_OUTSIDE_RE = re.compile(r"^[ \t]*-[ \t]+\*\*(Decided|Instead of|Because|Mine|Revisit when):")
MINE_RE = re.compile(r"^(yes|no)([ \t.(]|$)")


def valid_iso(value):
    base = value.split("T", 1)[0]
    if not DATE_RE.match(base):
        return False
    year, month, day = int(base[:4]), int(base[5:7]), int(base[8:10])
    if month < 1 or month > 12:
        return False
    if month == 2:
        days = 29 if (year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)) else 28
    elif month in (4, 6, 9, 11):
        days = 30
    else:
        days = 31
    if day < 1 or day > days:
        return False
    if value == base:
        return True
    if not ISO_TIME_RE.match(value):
        return False
    rest = value[11:]
    hour, minute = int(rest[0:2]), int(rest[3:5])
    second = int(rest[6:8]) if rest[5:6] == ":" else 0
    if hour > 23 or minute > 59 or second > 59:
        return False
    m = re.search(r"[+-]", rest)
    if m:
        offset = rest[m.start() + 1:]
        if int(offset[0:2]) > 23 or int(offset[3:5]) > 59:
            return False
    return True


def decision_problems(lines):
    """[(line, message, suggestion)] for decisions.md content."""
    problems = []
    state = {"entry": False, "start": 0, "values": {}}

    def finish():
        if not state["entry"]:
            return
        for name in DECISION_FIELDS:
            if not state["values"].get(name):
                problems.append((state["start"], "missing or empty decision field %s" % name,
                                 "Add - **%s:** meaningful content." % name))

    in_comment = in_fence = entries = False
    seen = {}
    pending = ""
    for number, s in enumerate(lines, 1):
        if in_comment:
            if "-->" in s:
                in_comment = False
            continue
        if "<!--" in s:
            if "-->" not in s:
                in_comment = True
            continue
        if FENCE_RE.match(s):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if re.match(r"^##[ \t]+Entries[ \t]*$", s):
            finish()
            entries, state["entry"] = True, False
            continue
        if not entries:
            continue
        if re.match(r"^##[ \t]+[^#]", s):
            finish()
            state.update(entry=True, start=number, values={})
            seen, pending = {}, ""
            title = re.sub(r"^##[ \t]+", "", s)
            date = re.split(r"[ \t]", title, 1)[0]
            if not valid_iso(date) or not DECISION_HEADING_RE.match(title):
                problems.append((number, "malformed decision heading",
                                 "Use ## ISO-date - title with a valid ISO8601 date or timestamp."))
            continue
        if not state["entry"]:
            if DECISION_OUTSIDE_RE.match(s):
                problems.append((number, "decision field outside an entry",
                                 "Add ## ISO-date - title above it."))
            continue
        v = re.sub(r"^[ \t]*-[ \t]+", "", s.replace("**", ""), 1)
        key = v.split(":", 1)[0]
        if key in DECISION_FIELDS:
            seen[key] = seen.get(key, 0) + 1
            if seen[key] > 1:
                problems.append((number, "duplicate decision field %s" % key,
                                 "Keep one %s field per entry." % key))
            v = v.split(":", 1)[1].strip(" \t\r") if ":" in v else ""
            state["values"][key] = bool(v)
            pending = key
            if key == "Mine" and not MINE_RE.match(v):
                problems.append((number, "invalid Mine field",
                                 "Use yes for agent decisions, no for user decisions."))
        elif pending and re.match(r"^[ \t]+[^ \t]", s):
            state["values"][pending] = True
        elif s.strip(" \t\r"):
            pending = ""
    finish()
    if not entries:
        problems.append((1, "missing ## Entries section", "Add ## Entries before the decision log."))
    return problems


def cmd_validate(args):
    opts, code = parse_common(args, VALIDATE_USAGE, ("strict",))
    if code is not None:
        return code
    validator = Validator(project_root())
    validator.run()
    errors, warnings = validator.errors, validator.warnings
    if errors or (opts["strict"] and warnings):
        status = 2
    elif warnings:
        status = 1
    else:
        status = 0
    if opts["format"] == "json":
        out = {"valid": status == 0 and errors == 0 and warnings == 0, "errors": errors,
               "warnings": warnings, "tasks": len(validator.files), "issues": validator.issues}
        sys.stdout.write(json.dumps(out, ensure_ascii=False, separators=(",", ":")) + "\n")
        return status
    color = colors_enabled(opts["no_color"])
    red, yellow, blue, off = ("\033[31m", "\033[33m", "\033[34m", "\033[0m") if color else ("",) * 4
    out = []
    for item in validator.issues:
        warn = item["severity"] == "WARNING"
        out.append("%s%s:%s %s:%d: %s\n  Fix: %s\n" % (
            yellow if warn else red, "Warning" if warn else "Error", off,
            item["file"], item["line"], item["message"], item["suggestion"]))
    if status == 0:
        out.append("%sWorkflow is valid (%d task files).%s\n" % (blue, len(validator.files), off))
    else:
        out.append("%d error(s), %d warning(s).\n" % (errors, warnings))
    sys.stdout.write("".join(out))
    return status


# ------------------------------------------------------------------- next

NEXT_USAGE = """workflow next - suggest what to work on next

Usage: workflow next [--format text|json] [--no-color]

Shows the top five ready tasks: not started, and every blocker archived under
.workflow/done/ ("Blocked by: None" also accepts "None, can start now"), with
their effort goal and Check preview. Efforts rank by the **Priority:** of
.workflow/specs/<effort>.md: critical, high, normal (default), low; ties sort
by task number.

Started tasks are listed above them, never as ready: paused, needing replanning
(Status: blocked), in progress, or finished on a branch but not landed. A task
counts as started when its file in this tree has a Status, or when a task branch
<effort>/<NN>-<slug> exists locally or on a remote (under pr, git fetch first);
the branch's copy of the task file gives its Status.

Options:
  --format TYPE  Output text (default) or JSON
  --no-color     Disable terminal colors
  -h, --help     Show this help
"""


def spec_info(wf, effort, cache):
    """(priority, rank, goal) for an effort from specs/<effort>.md."""
    if effort in cache:
        return cache[effort]
    priority, goal = "normal", "(not specified)"
    path = os.path.join(wf, "specs", effort + ".md")
    if effort != "none" and os.path.isfile(path):
        lines = read_lines(path)
        priority = read_fields(path, lines).get("priority", "normal").lower() or "normal"
        goal = spec_goal(lines) or goal
    if priority not in PRIORITY_RANK:
        sys.stderr.write("Warning: unknown priority '%s' for effort %s; using normal\n" % (priority, effort))
        priority = "normal"
    cache[effort] = (priority, PRIORITY_RANK[priority], goal)
    return cache[effort]


def spec_goal(lines):
    """First non-empty line under `## What this is for`."""
    in_fence = in_comment = inside = False
    for line in lines:
        line, in_comment = strip_comments(line, in_comment)
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        text = line.strip()
        if text.startswith("#"):
            if inside:
                return ""
            m = H2_RE.match(text)
            if m and m.group(1).strip().rstrip(":").strip().lower() == "what this is for":
                inside = True
            continue
        if inside and text:
            return text
    return ""


def spec_check_ids(wf, effort):
    """Check IDs ({'W1', ...}) the effort's spec defines, or an empty set.

    IDs are the bolded labels of the bullets under `## How we will know it
    works`: `- **W1.** <what command, what result>`. A spec without that section,
    or one that lists no IDs, yields an empty set and disables the check.
    """
    path = os.path.join(wf, "specs", effort + ".md")
    if effort == "none" or not os.path.isfile(path):
        return set()
    found = set()
    in_fence = in_comment = inside = False
    for line in read_lines(path):
        line, in_comment = strip_comments(line, in_comment)
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        text = line.strip()
        if text.startswith("#"):
            m = H2_RE.match(text)
            if m:
                title = m.group(1).strip().rstrip(":").strip().lower()
                inside = title == "how we will know it works"
            continue
        if inside:
            m = SPEC_CHECK_RE.match(text)
            if m:
                found.add(m.group(1))
    return found


# A started task's file lives on its branch until it lands (merge-strategy.md), so
# `next` reads task branches to tell claimed, paused and blocked tasks from ready ones.
TASK_BRANCH_RE = re.compile(r"^([a-z0-9]+(?:-[a-z0-9]+)*)/([0-9]+)-([^/]+)$")
STARTED_ORDER = {"paused": 0, "blocked": 1, "active": 2, "finished": 3}


def git_out(root, *args):
    """stdout of a git command run in root, or None when it fails."""
    try:
        proc = subprocess.run(["git", "-C", root] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, universal_newlines=True)
    except OSError:
        return None
    return proc.stdout if proc.returncode == 0 else None


def strip_remote_prefix(rest, remotes):
    """Strip the remote prefix from refs/remotes/<rest>, supporting multi-slash remotes."""
    for remote in remotes:
        prefix = remote + "/"
        if rest.startswith(prefix):
            return rest[len(prefix):]
    parts = rest.split("/")
    if len(parts) >= 3:
        return "/".join(parts[-2:])
    return parts[1] if len(parts) == 2 else ""


def task_branches(root):
    """{task ref: [(refname, branch), ...]} for <effort>/<NN>-<slug> branches.

    Local branches win over remote ones; among remotes, newer commits win.
    The checked-out branch is skipped because the working tree already shows its task file."""
    listing = git_out(root, "for-each-ref", "--sort=-committerdate",
                      "--format=%(refname)", "refs/heads", "refs/remotes")
    if not listing:
        return {}
    current = (git_out(root, "symbolic-ref", "-q", "--short", "HEAD") or "").strip()
    raw_remotes = (git_out(root, "remote") or "").splitlines()
    remotes = sorted((r.strip() for r in raw_remotes if r.strip()), key=len, reverse=True)
    heads, remote_refs = [], []
    for refname in listing.split():
        if refname.startswith("refs/heads/"):
            heads.append((refname, refname[len("refs/heads/"):]))
        elif refname.startswith("refs/remotes/"):
            name = strip_remote_prefix(refname[len("refs/remotes/"):], remotes)
            if name and name != "HEAD" and not name.endswith("/HEAD"):
                remote_refs.append((refname, name))
    found = {}
    for refname, name in heads + remote_refs:
        m = TASK_BRANCH_RE.match(name)
        if not m or name == current:
            continue
        ref = task_ref(m.group(1), norm_number(m.group(2)))
        found.setdefault(ref, []).append((refname, name))
    return found


def all_branch_names(root):
    """Every local and remote branch name, newest first."""
    listing = git_out(root, "for-each-ref", "--sort=-committerdate", "--format=%(refname)",
                      "refs/heads", "refs/remotes")
    if not listing:
        return []
    raw_remotes = (git_out(root, "remote") or "").splitlines()
    remotes = sorted((r.strip() for r in raw_remotes if r.strip()), key=len, reverse=True)
    names = []
    for refname in listing.split():
        if refname.startswith("refs/heads/"):
            names.append(refname[len("refs/heads/"):])
        elif refname.startswith("refs/remotes/"):
            name = strip_remote_prefix(refname[len("refs/remotes/"):], remotes)
            if name and name != "HEAD" and not name.endswith("/HEAD"):
                names.append(name)
    return names


def branch_task_status(root, refname, branch):
    """active | paused | blocked | finished, read from the task file on a branch."""
    effort, rest = branch.split("/", 1)
    for rel in (".workflow/tasks/%s/%s.md" % (effort, rest), ".workflow/tasks/%s.md" % rest):
        text = git_out(root, "show", "%s:%s" % (refname, rel))
        if text is not None:
            lines = [line[:-1] if line.endswith("\r") else line for line in text.split("\n")]
            status = read_fields(rel, lines).get("status").lower()
            return status if status in ("paused", "blocked") else "active"
    if git_out(root, "cat-file", "-e", "%s:.workflow/done/%s/%s.md" % (refname, effort, rest)) is not None:
        return "finished"
    return "active"


def branch_archived_tasks(root, branch, cache):
    """Set of task refs archived under .workflow/done/ on `branch` (for stacked tasks)."""
    if branch in cache:
        return cache[branch]
    found = set()
    target_ref = None
    for candidate in (branch, "origin/" + branch):
        if git_out(root, "rev-parse", "--verify", "--quiet", "--end-of-options", candidate + "^{commit}") is not None:
            target_ref = candidate
            break
    if target_ref:
        tree = git_out(root, "ls-tree", "-r", "--name-only", target_ref, ".workflow/done")
        if tree:
            for rel in tree.splitlines():
                parts = rel.split("/")
                if len(parts) >= 4 and parts[0] == ".workflow" and parts[1] == "done":
                    m = TASK_FILE_RE.match(parts[-1])
                    if m:
                        found.add(task_ref(parts[2], norm_number(m.group(1))))
    cache[branch] = found
    return found


def cmd_next(args):
    opts, code = parse_common(args, NEXT_USAGE, ())
    if code is not None:
        return code
    root = project_root()
    wf = os.path.join(root, ".workflow")
    tasks_dir, done_dir = os.path.join(wf, "tasks"), os.path.join(wf, "done")
    if not os.path.isdir(wf):
        sys.stderr.write("Error: no .workflow directory at %s; run 'workflow init' first\n" % root)
        return 1
    as_json = opts["format"] == "json"
    if not os.path.isdir(tasks_dir):
        if as_json:
            sys.stdout.write('{"ready_count":0,"task_count":0,"blocked_count":0,'
                             '"ready_tasks":[],"blockers":[]}\n')
        else:
            sys.stdout.write("No tasks found (tasks directory is missing).\n")
        return 0

    # Blockers archived under done/ are finished and never block.
    archived = set()
    for path in task_files(done_dir):
        m = TASK_FILE_RE.match(os.path.basename(path))
        if not m:
            continue
        effort = read_fields(path).get("effort") or dir_effort(done_dir, path)
        if not effort:
            continue
        archived.add(task_ref(effort, norm_number(m.group(1))))

    branches = task_branches(root)
    branch_status_cache = {}
    finished_branches = {}
    for b_ref, candidates in branches.items():
        for b_refname, b_name in candidates:
            b_status = branch_task_status(root, b_refname, b_name)
            branch_status_cache[(b_ref, b_name)] = b_status
            if b_status == "finished" and b_ref not in finished_branches:
                finished_branches[b_ref] = b_name
    # A task's archive may sit on a branch that is not named after it (an integration
    # or aggregate branch), so union every branch's done/ tree and record who carries it.
    union_cache = {}
    archived_branch = {}
    for b_name in all_branch_names(root):
        for carried in branch_archived_tasks(root, b_name, union_cache):
            archived_branch.setdefault(carried, b_name)

    ready, started, stackable = [], [], []
    block_tasks, block_numbers = {}, {}
    task_count = blocked_count = waiting = 0
    specs = {}
    base_archived_cache = {}

    def record(blocker, ref, number):
        block_tasks.setdefault(blocker, []).append(ref)
        block_numbers.setdefault(blocker, []).append(int(number))

    for path in task_files(tasks_dir):
        name = os.path.basename(path)
        m = TASK_FILE_RE.match(name)
        if not m:
            sys.stderr.write("Warning: ignoring task with invalid filename: %s\n" % name)
            continue
        number = norm_number(m.group(1))
        meta = read_fields(path)
        status = meta.get("status").lower()
        if status in ("done", "completed", "archived"):
            continue
        effort = meta.get("effort") or dir_effort(tasks_dir, path) or "none"
        if not SLUG_RE.match(effort):
            sys.stderr.write("Warning: %s has an invalid effort reference; using default priority\n" % name)
            effort = "none"
        task_count += 1
        ref = task_ref(effort, number)
        title = meta.title or meta.get("task") or "Untitled"

        # Started here (Status in this tree) or on its own task branch: not ready.
        branch = None
        if status not in ("active", "paused", "blocked"):
            if ref in finished_branches:
                status, branch = "finished", finished_branches[ref]
            elif ref in archived_branch:
                status, branch = "finished", archived_branch[ref]
            elif ref in branches:
                branch = branches[ref][0][1]
                status = branch_status_cache[(ref, branch)]
        if status in STARTED_ORDER:
            started.append((STARTED_ORDER[status], int(number), ref, title, status, branch, effort))
            if status == "blocked":
                blocked_count += 1
            continue

        base_branch = meta.get("base branch").strip()
        stacked_done = set()
        if base_branch and not (base_branch.startswith("<") and base_branch.endswith(">")):
            stacked_done = branch_archived_tasks(root, base_branch, base_archived_cache)

        blocked = False
        raw = meta.get("blocked by")
        if not raw:
            blocked = True
            record("(missing Blocked by field)", ref, number)
        else:
            try:
                open_refs = []
                for token, _ in parse_blockers(raw):
                    target = resolve_ref(token, effort)
                    if target not in archived and target not in stacked_done and target not in open_refs:
                        open_refs.append(target)
                for target in open_refs:
                    record(target, ref, number)
                blocked = bool(open_refs)
                if open_refs and all(t in finished_branches for t in open_refs):
                    stackable.append((ref, [finished_branches[t] for t in open_refs]))
            except BlockerError:
                blocked = True
                sys.stderr.write("Warning: %s has malformed Blocked by: %s\n" % (name, raw))
                record("(malformed Blocked by field)", ref, number)
        if blocked:
            blocked_count += 1
            waiting += 1
            continue
        priority, rank, goal = spec_info(wf, effort, specs)
        task = {"number": int(number), "title": title, "effort": effort, "priority": priority,
                "goal": goal, "check": meta.get("check") or "(not specified)",
                "start_command": "flow-implement " + ref}
        ready.append((rank, int(number), len(ready), ref, task))
    ready.sort(key=lambda item: item[:3])
    started.sort(key=lambda item: (item[0], item[6], item[1]))
    blockers = sorted(block_tasks, key=lambda b: (-len(block_tasks[b]), b.encode("utf-8")))[:5]

    if as_json:
        out = {"ready_count": len(ready), "task_count": task_count, "blocked_count": blocked_count,
               "ready_tasks": [item[4] for item in ready[:5]],
               "blockers": [{"dependency": b, "count": len(block_tasks[b]), "tasks": block_numbers[b]}
                            for b in blockers],
               "in_progress": [{"task": item[2], "number": item[1], "effort": item[6], "title": item[3],
                                "status": item[4], "branch": item[5]} for item in started]}
        sys.stdout.write(json.dumps(out, ensure_ascii=False, separators=(",", ":")) + "\n")
        return 0

    color = colors_enabled(opts["no_color"])
    bold, dim, off = ("\033[1m", "\033[2m", "\033[0m") if color else ("", "", "")
    out = []
    if task_count == 0:
        sys.stdout.write("No tasks found.\n")
        return 0
    headings = (("paused", "Paused (resume before starting new work):"),
                ("blocked", "Needs replanning (Status: blocked; flow-break amend):"),
                ("active", "In progress:"),
                ("finished", "Finished on a branch, not landed yet:"))
    for state, heading in headings:
        rows = [item for item in started if item[4] == state]
        if rows:
            out.append("%s\n" % heading)
            for item in rows:
                where = "branch %s" % item[5] if item[5] else "this working tree"
                out.append("  %s%s%s %s (%s)\n" % (bold, item[2], off, item[3], where))
    if ready:
        out.append("%sReady tasks%s (%d total; showing up to 5):\n" % (bold, off, len(ready)))
        for i, item in enumerate(ready[:5], 1):
            ref, task = item[3], item[4]
            out.append("%d) %s%s%s %s (priority: %s)\n" % (i, bold, ref, off, task["title"], task["priority"]))
            if task["goal"] != "(not specified)":
                out.append("   %sGoal:%s %s\n" % (dim, off, task["goal"]))
            out.append("   %sCheck:%s %s\n" % (dim, off, task["check"]))
            out.append("   Start: flow-implement %s\n" % ref)
    else:
        out.append("No ready tasks.\n")
    if waiting:
        out.append("Waiting on dependencies: %d task(s); top blockers: %s\n" % (
            waiting, ", ".join("%s (%d)" % (b, len(block_tasks[b])) for b in blockers)))
        if stackable:
            out.append("Stackable under pr (blockers finished on branch): %s\n" % (
                ", ".join("%s (on %s)" % (r, ", ".join(bs)) for r, bs in stackable)))
    sys.stdout.write("".join(out))
    return 0


# ------------------------------------------------------------------- main

MAIN_USAGE = """workflow-core.py %s - validate and next for the workflow toolkit

Usage: workflow-core.py validate [--strict] [--format text|json] [--no-color]
       workflow-core.py next [--format text|json] [--no-color]
       workflow-core.py <command> --help
""" % VERSION


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        (sys.stdout if argv else sys.stderr).write(MAIN_USAGE)
        return 0 if argv else 2
    command, args = argv[0], argv[1:]
    if command == "validate":
        return cmd_validate(args)
    if command == "next":
        return cmd_next(args)
    sys.stderr.write("Error: unknown command: %s\n" % command)
    sys.stderr.write(MAIN_USAGE)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BrokenPipeError:
        sys.exit(0)
    except KeyboardInterrupt:
        sys.exit(130)
