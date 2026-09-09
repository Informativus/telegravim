"""Trusted PR inspection. Contributor files are read as data, never executed."""

import argparse
import difflib
import hashlib
import html
import json
import os
import posixpath
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
POLICY = json.loads((HERE / "policy.json").read_text())
MARKER = "<!-- telegravim-pr-security -->"
SECURITY_STATUS = "Telegravim / security"
NETWORK_STATUS = "Telegravim / owner network review"
SHA = re.compile(r"[0-9a-f]{40}")
NETWORK = re.compile(
    r"\b(?:QNetwork\w*|Q\w*Socket|QTcp\w*|QUdp\w*|"
    r"curl_\w+|SSL_\w+|MTP[A-Z]\w*|fetch|XMLHttpRequest|WebSocket|"
    r"connectToHost|sendto|recvfrom|getaddrinfo|socket|WinHttp\w*|"
    r"WinInet\w*|URLSession|NSURLSession)\b|"
    r"\b(?:api|_api|mtp)\s*(?:\([^)]*\))?\s*(?:\.|->)\s*request\s*\(",
    re.IGNORECASE,
)
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".m",
    ".mm",
}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}
DOCUMENT_NAMES = {"README.md", "LICENSE", "NOTICE", "LEGAL", "COPYING"}


class GuardError(RuntimeError):
    pass


def safe_text(value: str, limit: int = 240) -> str:
    text = " ".join(value.split())[:limit]
    text = "".join(c for c in text if c.isprintable())
    return html.escape(text).replace("@", "&#64;").replace("`", "&#96;")


def valid_sha(value: str) -> str:
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise GuardError("Invalid commit identifier")
    return value


def safe_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or not value
        or any(p in {".", "..", ".git"} for p in path.parts)
        or "\\" in value
        or any(ord(c) < 32 for c in value)
    ):
        raise GuardError("Unsupported file path; manual inspection required")
    return path


def control_path(path: str) -> bool:
    return path.startswith(".github/") or path in {
        ".gitmodules",
        ".gitattributes",
        ".gitignore",
    }


def documentation(path: str, mode: str) -> bool:
    return mode == "100644" and (
        path in DOCUMENT_NAMES
        or (path.startswith("docs/") and PurePosixPath(path).suffix in {".md", ".txt"})
    )


def classify(
    path: str,
    old_mode: str,
    new_mode: str,
    old_blob: str,
    new_blob: str,
    upstream: tuple[str, str] | None,
    before: bytes,
    after: bytes,
) -> str:
    if control_path(path):
        return "Review policy, workflow, or dependency control changed"
    if old_mode not in {"000000", "100644", "100755"} or new_mode not in {
        "000000",
        "100644",
        "100755",
    }:
        return "Submodule, symlink, or unsupported object changed"
    if old_mode != new_mode and old_mode != "000000" and new_mode != "000000":
        return "Executable mode changed"
    if new_mode != "000000" and upstream == (new_mode, new_blob):
        return ""
    if documentation(path, new_mode) and old_mode in {"000000", "100644"}:
        return ""
    if new_mode == "000000" and documentation(path, old_mode):
        return ""
    before_text = before.decode("utf-8", errors="replace")
    after_text = after.decode("utf-8", errors="replace")
    changed_lines = "\n".join(
        line[1:]
        for line in difflib.unified_diff(
            before_text.splitlines(), after_text.splitlines()
        )
        if line.startswith(("+", "-")) and not line.startswith(("+++", "---"))
    )
    if NETWORK.search(before_text + "\n" + after_text) or re.search(
        r"https?://|wss?://", changed_lines
    ):
        return "Network-related code outside verified Telegram upstream changed"
    return "Custom executable code or data changed; network effects cannot be excluded"


def command(
    args: list[str],
    *,
    cwd: Path | None = None,
    input_data: bytes | None = None,
    limit: int = 8_000_000,
    timeout: int = 180,
    allowed: tuple[int, ...] = (0,),
) -> bytes:
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        result = subprocess.run(
            args,
            cwd=cwd,
            input=input_data,
            stdout=stdout,
            stderr=stderr,
            timeout=timeout,
            check=False,
        )
        if result.returncode not in allowed:
            raise GuardError(
                f"{Path(args[0]).name} failed (exit {result.returncode}); no source output logged"
            )
        if stdout.tell() > limit:
            raise GuardError(
                f"{Path(args[0]).name} output limit exceeded; inspection incomplete"
            )
        stdout.seek(0)
        return stdout.read()


def git(repo: Path, *args: str, limit: int = 8_000_000) -> bytes:
    return command(
        [
            "git",
            "--no-pager",
            "-c",
            "core.hooksPath=/dev/null",
            "-c",
            "protocol.file.allow=never",
            "-c",
            "protocol.ext.allow=never",
            "-C",
            str(repo),
            *args,
        ],
        limit=limit,
    )


class GitHub:
    def __init__(self):
        self.repository = POLICY["repository"]
        if os.environ.get("GITHUB_REPOSITORY") != self.repository:
            raise GuardError("Unexpected repository")
        self.token = os.environ["GITHUB_TOKEN"]

    def request(self, path: str, data=None, method: str = "GET"):
        if not path.startswith(f"/repos/{self.repository}/"):
            raise GuardError("Unexpected GitHub API path")
        body = json.dumps(data).encode() if data is not None else None
        request = urllib.request.Request(
            "https://api.github.com" + path,
            data=body,
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
                "Content-Type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=45) as response:
                raw = response.read(8_000_001)
                if len(raw) > 8_000_000:
                    raise GuardError("GitHub response too large")
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as error:
            raise GuardError(f"GitHub API request failed (HTTP {error.code})") from None

    def repo(self, suffix: str, data=None, method: str = "GET"):
        return self.request(f"/repos/{self.repository}/{suffix}", data, method)

    def status(self, head: str, context: str, state: str, description: str):
        self.repo(
            f"statuses/{valid_sha(head)}",
            {
                "context": context,
                "state": state,
                "description": description[:140],
                "target_url": f"https://github.com/{self.repository}/actions/runs/{os.environ['GITHUB_RUN_ID']}",
            },
            "POST",
        )

    def comments(self, number: int):
        for page in range(1, 21):
            batch = self.repo(f"issues/{number}/comments?per_page=100&page={page}")
            yield from batch
            if len(batch) < 100:
                return
        raise GuardError("Comment limit exceeded")

    def comment(self, number: int, body: str):
        existing = [
            c
            for c in self.comments(number)
            if c["user"]["login"] == "github-actions[bot]"
            and c["body"].startswith(MARKER)
        ]
        if existing:
            self.repo(f"issues/comments/{existing[-1]['id']}", {"body": body}, "PATCH")
        else:
            self.repo(f"issues/{number}/comments", {"body": body}, "POST")


def output(name: str, value) -> None:
    rendered = str(value).lower() if isinstance(value, bool) else str(value)
    if "\n" in rendered or "\r" in rendered:
        raise GuardError("Invalid workflow output")
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as target:
            target.write(f"{name}={rendered}\n")


def verify_environment(api: GitHub):
    environment = api.repo(f"environments/{POLICY['environment']}")
    rules = [
        rule
        for rule in environment.get("protection_rules", [])
        if rule.get("type") == "required_reviewers"
    ]
    reviewers = rules[0].get("reviewers", []) if len(rules) == 1 else []
    if (
        len(reviewers) != 1
        or reviewers[0].get("type") != "User"
        or reviewers[0].get("reviewer", {}).get("login") != POLICY["owner"]
        or rules[0].get("prevent_self_review", True)
        or environment.get("can_admins_bypass", True)
    ):
        raise GuardError(
            "network-review must require only Informativus, allow self-review, and forbid admin bypass"
        )


def current_pr(api: GitHub, number: int):
    pr = api.repo(f"pulls/{number}")
    if pr["state"] != "open" or pr["base"]["ref"] not in {"main", "develop"}:
        raise GuardError("Only open PRs targeting main or develop are supported")
    if pr["base"]["repo"]["full_name"] != POLICY["repository"]:
        raise GuardError("Unexpected base repository")
    valid_sha(pr["head"]["sha"])
    pr["base"]["sha"] = valid_sha(
        api.repo(f"branches/{pr['base']['ref']}")["commit"]["sha"]
    )
    return pr


def same_snapshot(pr, state) -> bool:
    return pr["head"]["sha"] == state["head"] and pr["base"]["sha"] == state["base"]


def read_blob(repo: Path, blob: str, maximum: int) -> bytes:
    if blob == "0" * 40:
        return b""
    valid_sha(blob)
    size = int(git(repo, "cat-file", "-s", blob))
    if size > maximum:
        raise GuardError("Changed blob exceeds inspection limit")
    return git(repo, "cat-file", "blob", blob, limit=maximum)


def tree(repo: Path, ref: str) -> dict[str, tuple[str, str]]:
    result = {}
    for record in git(repo, "ls-tree", "-r", "-z", valid_sha(ref)).split(b"\0"):
        if not record:
            continue
        metadata, path = record.split(b"\t", 1)
        mode, _, blob = metadata.decode().split()
        result[path.decode("utf-8")] = (mode, blob)
    return result


def prepare_repository(
    root: Path, number: int, head: str, base: str
) -> tuple[Path, str, str]:
    repo = root / "objects.git"
    command(["git", "init", "--bare", str(repo)])
    git(
        repo,
        "remote",
        "add",
        "origin",
        f"https://github.com/{POLICY['repository']}.git",
    )
    git(
        repo,
        "fetch",
        "--no-tags",
        "--depth=512",
        "origin",
        f"+refs/pull/{number}/head:refs/guard/head",
        base,
    )
    if git(repo, "rev-parse", "refs/guard/head").decode().strip() != head:
        raise GuardError("PR changed while fetching; rerun the workflow")
    merge_base = valid_sha(git(repo, "merge-base", base, head).decode().strip())
    git(repo, "remote", "add", "official", POLICY["upstream"])
    git(repo, "config", "remote.official.promisor", "true")
    git(repo, "config", "remote.official.partialclonefilter", "blob:none")
    git(
        repo,
        "fetch",
        "--no-tags",
        "--depth=1",
        "--filter=blob:none",
        "official",
        f"+{POLICY['upstream_ref']}:refs/guard/upstream",
    )
    upstream = valid_sha(git(repo, "rev-parse", "refs/guard/upstream").decode().strip())
    return repo, merge_base, upstream


def scanner(root: Path, name: str, args: list[str], input_data: bytes | None = None):
    executable = str(Path(os.environ["SCANNER_BIN"]).resolve() / name)
    command(
        [executable, *args], cwd=root, input_data=input_data, limit=100_000, timeout=300
    )


def check_workflows(root: Path, workflows: list[str]):
    scanner(root, "actionlint", ["-oneline", "-shellcheck=", "-pyflakes=", *workflows])
    executable = str(Path(os.environ["SCANNER_BIN"]).resolve() / "zizmor")
    raw = command(
        [
            executable,
            "--offline",
            "--no-config",
            "--no-ignores",
            "--strict-collection",
            "--format",
            "json",
            "--no-exit-codes",
            "--min-severity",
            "medium",
            "--min-confidence",
            "medium",
            *workflows,
        ],
        cwd=root,
        limit=2_000_000,
    )
    findings = json.loads(raw)
    for finding in findings:
        locations = finding.get("locations", [])
        paths = [
            location.get("symbolic", {})
            .get("key", {})
            .get("Local", {})
            .get("verbatim_path", "")
            for location in locations
        ]
        accepted_trigger = (
            finding["ident"] == "dangerous-triggers"
            and paths
            and all(
                path.endswith("/.github/workflows/pr-security.yml")
                or path == ".github/workflows/pr-security.yml"
                for path in paths
            )
        )
        if not accepted_trigger:
            raise GuardError(f"Workflow security finding: {finding['ident']}")


def inspect(repo: Path, root: Path, head: str, merge_base: str, upstream: str) -> dict:
    records = git(
        repo,
        "diff",
        "--raw",
        "--no-abbrev",
        "--no-renames",
        "-z",
        merge_base,
        head,
        "--",
    ).split(b"\0")
    if records[-1] == b"":
        records.pop()
    if len(records) % 2 or len(records) // 2 > POLICY["max_files"]:
        raise GuardError("Changed-file limit exceeded or invalid diff")
    upstream_tree = tree(repo, upstream)
    reasons, changed = [], []
    workflow_root = root / "workflows"
    workflow_root.mkdir()
    for offset in range(0, len(records), 2):
        old_mode, new_mode, old_blob, new_blob, _ = records[offset].decode()[1:].split()
        path = records[offset + 1].decode("utf-8")
        safe_path(path)
        before = (
            read_blob(repo, old_blob, POLICY["max_blob_bytes"])
            if old_mode.startswith("100")
            else b""
        )
        after = (
            read_blob(repo, new_blob, POLICY["max_blob_bytes"])
            if new_mode.startswith("100")
            else b""
        )
        reason = classify(
            path,
            old_mode,
            new_mode,
            old_blob,
            new_blob,
            upstream_tree.get(path),
            before,
            after,
        )
        changed.append(path)
        if reason:
            reasons.append({"path": path, "reason": reason})
        if (
            path.startswith(".github/workflows/")
            and path.endswith((".yml", ".yaml"))
            and new_mode != "000000"
        ):
            if new_mode != "100644":
                raise GuardError("Workflow must be a regular non-executable file")
            target = workflow_root / safe_path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(after)
    diff = git(
        repo,
        "diff",
        "--no-ext-diff",
        "--no-textconv",
        "--no-renames",
        "--unified=12",
        merge_base,
        head,
        "--",
        limit=POLICY["max_diff_bytes"],
    )
    commits = int(git(repo, "rev-list", "--count", f"{merge_base}..{head}"))
    if commits > POLICY["max_commits"]:
        raise GuardError(
            "Commit limit exceeded; split the PR for complete secret scanning"
        )
    history = git(
        repo,
        "log",
        "--format=",
        "-p",
        "--no-ext-diff",
        "--no-textconv",
        "--diff-merges=first-parent",
        f"{merge_base}..{head}",
        "--",
        limit=POLICY["max_history_bytes"],
    )
    scan_ok, checks = True, []
    try:
        scanner(
            root,
            "gitleaks",
            [
                "stdin",
                "--config",
                str(HERE / "gitleaks.toml"),
                "--gitleaks-ignore-path",
                "/dev/null",
                "--ignore-gitleaks-allow",
                "--redact=100",
                "--no-banner",
            ],
            diff + b"\n" + history,
        )
        checks.append("Secrets: passed (PR diff and introduced commit patches)")
    except GuardError:
        scan_ok = False
        checks.append(
            "Secrets: failed or incomplete; inspect the diff locally with gitleaks --redact"
        )
    workflows = sorted(str(p) for p in workflow_root.rglob("*.y*ml"))
    if workflows:
        try:
            check_workflows(root, workflows)
            checks.append("Workflow syntax and security: passed")
        except GuardError as error:
            scan_ok = False
            checks.append(
                f"Workflow syntax/security: {safe_text(str(error))}; see docs/pr-security.md for reproduction"
            )
    else:
        checks.append("Workflow syntax and security: no active workflow changed")
    try:
        git(repo, "diff", "--check", merge_base, head, "--")
        checks.append("Diff whitespace/conflict-marker check: passed")
    except GuardError:
        scan_ok = False
        checks.append("Diff whitespace/conflict-marker check: failed")
    return {
        "reasons": reasons,
        "changed": changed,
        "scan_ok": scan_ok,
        "checks": checks,
    }


def export_paths(repo: Path, head: str, changed: list[str]):
    entries = {
        path: value
        for path, value in tree(repo, head).items()
        if value[0] in {"100644", "100755"}
        and PurePosixPath(path).suffix in SOURCE_SUFFIXES
        and not path.startswith("Telegram/ThirdParty/")
    }
    relevant = [
        path
        for path in changed
        if not documentation(path, "100644")
        and (
            not path.startswith(".github/")
            or PurePosixPath(path).suffix in SOURCE_SUFFIXES
        )
    ]
    narrow = (
        relevant
        and all(
            PurePosixPath(path).suffix in SOURCE_SUFFIXES - HEADER_SUFFIXES
            for path in relevant
        )
        and any(path in entries for path in relevant)
    )
    if not narrow:
        return entries, "full source tree"
    suffixes = {}
    for path in entries:
        parts = PurePosixPath(path).parts
        for index in range(len(parts)):
            suffixes.setdefault("/".join(parts[index:]), set()).add(path)
    selected = {path for path in relevant if path in entries}
    pending = list(selected)
    while pending:
        path = pending.pop()
        content = read_blob(repo, entries[path][1], 10_000_000).decode(
            "utf-8", errors="replace"
        )
        for include in re.findall(
            r'^\s*#\s*include\s*[<"]([^">]+)[">]', content, re.MULTILINE
        ):
            local = posixpath.normpath(str(PurePosixPath(path).parent / include))
            candidates = {local} if local in entries else suffixes.get(include, set())
            for candidate in candidates - selected:
                selected.add(candidate)
                pending.append(candidate)
    return {
        path: entries[path] for path in sorted(selected)
    }, "changed translation units and included source files"


def export_source(repo: Path, root: Path, head: str, changed: list[str]):
    entries, scope = export_paths(repo, head, changed)
    total, count = 0, 0
    for path, (mode, blob) in entries.items():
        destination = root / "source" / safe_path(path)
        data = read_blob(repo, blob, 10_000_000)
        count += 1
        total += len(data)
        if total > 150_000_000 or count > 20_000:
            raise GuardError("Source analysis export limit exceeded")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    if not count:
        raise GuardError("No C/C++ source exported; analysis cannot run")
    print(f"CodeQL scope: {scope}; {count} source files")


def codeql_required(changed: list[str]) -> bool:
    return any(
        PurePosixPath(path).suffix in SOURCE_SUFFIXES
        or (not documentation(path, "100644") and not path.startswith(".github/"))
        for path in changed
    )


def comment_body(state: dict) -> str:
    if state["reasons"]:
        network = (
            "**This change requires owner review by @Informativus.** "
            "See **Telegravim / owner network review** for the current approval status. "
            "If pending, open the run and use **Review deployments → network-review → Approve** "
            "after inspecting the diff. Each new push requires a fresh approval."
        )
    else:
        network = "Owner network review: no custom executable/data changes found (verified upstream or documentation)."
    lines = [
        MARKER,
        f"Commit: `{state['head']}` · base: `{state['base']}`",
        "",
        network,
        "",
    ]
    for item in state["reasons"][:12]:
        lines.append(f"- {safe_text(item['path'])}: {item['reason']}")
    if len(state["reasons"]) > 12:
        lines.append(f"- {len(state['reasons']) - 12} more files; see the job summary.")
    lines.extend(
        [
            "",
            *state["checks"],
            "",
            f"Official Telegram comparison: `{state['upstream']}`.",
        ]
    )
    return "\n".join(lines)


def run_audit(api: GitHub, number: int, root: Path):
    pr = current_pr(api, number)
    head, base = pr["head"]["sha"], pr["base"]["sha"]
    output("number", number)
    output("head", head)
    output("base", base)
    for context in [SECURITY_STATUS, NETWORK_STATUS]:
        api.status(
            head,
            context,
            "pending",
            "Inspection in progress; this is not merge approval",
        )
    try:
        root.mkdir(parents=True, exist_ok=False)
        repo, merge_base, upstream = prepare_repository(root, number, head, base)
        state = inspect(repo, root, head, merge_base, upstream)
        state.update(
            number=number,
            head=head,
            base=base,
            upstream=upstream,
            control=valid_sha(os.environ["CONTROL_SHA"]),
        )
        if state["reasons"]:
            verify_environment(api)
        if not same_snapshot(current_pr(api, number), state):
            raise GuardError(
                "PR changed during inspection; rerun on the current revision"
            )
        (root / "state.json").write_text(json.dumps(state))
        api.comment(number, comment_body(state))
        if os.environ.get("GITHUB_STEP_SUMMARY"):
            with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as summary:
                summary.write(comment_body(state) + "\n\n")
                for item in state["reasons"]:
                    summary.write(f"- {safe_text(item['path'])}: {item['reason']}\n")
        output("requires_review", bool(state["reasons"]))
        output("scan_ok", state["scan_ok"])
        output("codeql_required", codeql_required(state["changed"]))
        if state["scan_ok"] and codeql_required(state["changed"]):
            export_source(repo, root, head, state["changed"])
        elif not state["scan_ok"]:
            api.status(
                head,
                SECURITY_STATUS,
                "failure",
                "Secret, workflow or diff inspection failed/incomplete",
            )
    except Exception:
        for context in [SECURITY_STATUS, NETWORK_STATUS]:
            api.status(
                head,
                context,
                "failure",
                "Inspection failed or incomplete; rerun after resolving the error",
            )
        raise


def finish(api: GitHub, kind: str):
    state = {
        "head": valid_sha(os.environ["PR_HEAD"]),
        "base": valid_sha(os.environ["PR_BASE"]),
    }
    number = int(os.environ["PR_NUMBER"])
    pr = current_pr(api, number)
    fresh = same_snapshot(pr, state)
    control = api.repo("commits/main")["sha"]
    fresh = fresh and control == valid_sha(os.environ["CONTROL_SHA"])
    if kind == "network":
        verify_environment(api)
        required = os.environ.get("REQUIRES_REVIEW") == "true"
        approved = os.environ.get("APPROVAL_RESULT") == "success"
        passed = (
            fresh
            and os.environ.get("AUDIT_RESULT") == "success"
            and (not required or approved)
        )
        context = NETWORK_STATUS
        detail = (
            "Owner approved this exact revision"
            if required
            else "No owner network approval required"
        )
    else:
        required = os.environ.get("CODEQL_REQUIRED")
        analyzed = required == "true" and os.environ.get("CODEQL_RESULT") == "success"
        exempt = required == "false" and os.environ.get("CODEQL_RESULT") == "skipped"
        passed = (
            fresh
            and os.environ.get("AUDIT_RESULT") == "success"
            and os.environ.get("SCAN_OK") == "true"
            and (analyzed or exempt)
        )
        context = SECURITY_STATUS
        detail = (
            "Secret, workflow, diff and CodeQL checks passed"
            if analyzed
            else "Secret, workflow and diff checks passed; C/C++ sources unchanged"
        )
    api.status(
        state["head"],
        context,
        "success" if passed else "failure",
        detail
        if passed
        else "Checks failed, approval missing, or head/base/policy changed; rerun",
    )
    if not passed:
        raise GuardError("Required check did not pass")


def sarif_level(run: dict, result: dict) -> str:
    if "level" in result:
        level = result["level"]
    else:
        reference = result.get("rule", {})
        component = run.get("tool", {}).get("driver", {})
        if "toolComponent" in reference:
            index = reference["toolComponent"].get("index")
            extensions = run.get("tool", {}).get("extensions", [])
            if type(index) is not int or not 0 <= index < len(extensions):
                return "warning"
            component = extensions[index]
        rules = component.get("rules", [])
        rule_id = result.get("ruleId", reference.get("id"))
        index = reference.get("index", result.get("ruleIndex"))
        if type(index) is int and 0 <= index < len(rules):
            rule = rules[index]
            if rule_id and rule.get("id") != rule_id:
                return "warning"
        else:
            matches = [rule for rule in rules if rule_id and rule.get("id") == rule_id]
            if len(matches) != 1:
                return "warning"
            rule = matches[0]
        level = rule.get("defaultConfiguration", {}).get("level", "warning")
    return level if level in {"none", "note", "warning", "error"} else "warning"


def reviewed_finding(run: dict, result: dict, source: Path, reviews: list) -> bool:
    locations = result.get("locations", [])
    if len(locations) != 1:
        return False
    physical = locations[0].get("physicalLocation", {})
    path = physical.get("artifactLocation", {}).get("uri", "")
    try:
        relative = safe_path(path)
    except GuardError:
        return False
    if str(relative) != path:
        return False
    rule = result.get("ruleId")
    allowed = {
        "Telegram/SourceFiles/boxes/edit_caption_box.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/calls/calls_call.h": {
            "cpp/member-const-no-effect",
            "cpp/non-member-const-no-effect",
        },
        "Telegram/SourceFiles/chat_helpers/field_autocomplete.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/data/data_document.h": {
            "cpp/member-const-no-effect",
        },
        "Telegram/SourceFiles/editor/editor_paint.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/editor/photo_editor_controls.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/editor/scene/scene.cpp": {
            "cpp/ambiguously-signed-bit-field",
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/editor/scene/scene_item_base.cpp": {
            "cpp/virtual-call-in-constructor",
        },
        "Telegram/SourceFiles/editor/video/video_editor.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/history/history_drag_area.cpp": {
            "cpp/dead-code-goto",
        },
        "Telegram/SourceFiles/history/view/history_view_element.cpp": {
            "cpp/constant-comparison",
        },
        "Telegram/SourceFiles/history/view/history_view_list_widget.h": {
            "cpp/member-const-no-effect",
            "cpp/missing-return",
        },
        "Telegram/SourceFiles/history/view/history_view_reply.h": {
            "cpp/ambiguously-signed-bit-field",
        },
        "Telegram/SourceFiles/history/view/history_view_top_bar_widget.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/iv/markdown/iv_markdown_article.cpp": {
            "cpp/dead-code-goto",
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/iv/markdown/iv_markdown_article_layout_blocks.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/overview/overview_layout.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/overview/overview_layout.h": {
            "cpp/ambiguously-signed-bit-field",
            "cpp/virtual-destructor",
        },
        "Telegram/SourceFiles/settings/settings_experimental.cpp": {
            "cpp/ambiguously-signed-bit-field",
        },
        "Telegram/SourceFiles/storage/localimageloader.h": {
            "cpp/ambiguously-signed-bit-field",
            "cpp/member-const-no-effect",
            "cpp/missing-return",
        },
        "Telegram/SourceFiles/tests/test_vim_keymap.cpp": {
            "cpp/constant-comparison",
            "cpp/missing-return",
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/ui/chat/message_bar.cpp": {
            "cpp/poorly-documented-function",
        },
        "Telegram/SourceFiles/ui/controls/swipe_handler.cpp": {
            "cpp/dead-code-goto",
        },
    }
    if rule not in allowed.get(path, set()):
        return False
    fingerprint = result.get("partialFingerprints", {}).get("primaryLocationLineHash")
    if not fingerprint:
        return False
    message = hashlib.sha256(result.get("message", {}).get("text", "").encode()).hexdigest()
    candidates = [
        review for review in reviews
        if review["path"] == path and any(
            finding["rule"] == rule
            and finding["line_hash"] == fingerprint
            and finding["message_sha256"] == message
            and finding["level"] == sarif_level(run, result)
            and finding["reason"].strip()
            for finding in review["findings"]
        )
    ]
    if not candidates:
        return False
    file = source / relative
    if not file.is_file() or file.is_symlink() or source.resolve() not in file.resolve().parents:
        return False
    digest = hashlib.sha256(file.read_bytes()).hexdigest()
    return any(digest in review["source_sha256"] for review in candidates)


def check_sarif(
    directory: Path, changed: list[str] | None = None, source: Path | None = None
):
    files = list(directory.glob("*.sarif"))
    if not files:
        raise GuardError("CodeQL produced no SARIF; analysis incomplete")
    reviews = json.loads((HERE / "codeql-reviewed-findings.json").read_text()) if source else []
    errors = findings = reviewed = 0
    for path in files:
        document = json.loads(path.read_text())
        if not isinstance(document.get("runs"), list) or not document["runs"]:
            raise GuardError("Invalid or empty CodeQL SARIF; analysis incomplete")
        for run in document["runs"]:
            if any(
                not invocation.get("executionSuccessful", True)
                for invocation in run.get("invocations", [])
            ):
                errors += 1
            for result in run.get("results", []):
                if sarif_level(run, result) not in {"warning", "error"}:
                    continue
                locations = result.get("locations", [])
                paths = [
                    location.get("physicalLocation", {})
                    .get("artifactLocation", {})
                    .get("uri", "")
                    for location in locations
                ]
                if (
                    changed is None
                    or not paths
                    or any(
                        path == item or path.endswith("/" + item)
                        for path in paths
                        for item in changed
                    )
                ):
                    if source and reviewed_finding(run, result, source, reviews):
                        reviewed += 1
                    else:
                        findings += 1
    print(
        f"CodeQL: {findings} warning/error findings; {errors} failed invocations; "
        f"{reviewed} exact reviewed findings (retained in SARIF)"
    )
    if errors or findings:
        raise GuardError("CodeQL findings require review; see the SARIF artifact")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode", choices=["audit", "finish-network", "finish-security", "sarif"]
    )
    parser.add_argument("--state", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(os.environ.get("RUNNER_TEMP", "/tmp")) / "pr-inspection",
    )
    args = parser.parse_args()
    if args.mode == "sarif":
        changed = json.loads(args.state.read_text())["changed"] if args.state else None
        check_sarif(args.root, changed, args.source)
        return
    api = GitHub()
    if args.mode == "audit":
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
        number = event.get("number") or event.get("inputs", {}).get("pr_number")
        if not str(number).isdigit() or int(number) <= 0:
            raise GuardError("Invalid PR number")
        run_audit(api, int(number), args.root)
    else:
        finish(api, args.mode.removeprefix("finish-"))


if __name__ == "__main__":
    try:
        main()
    except (GuardError, subprocess.TimeoutExpired, UnicodeError) as error:
        print(f"::error::{safe_text(str(error))}", file=sys.stderr)
        sys.exit(1)
