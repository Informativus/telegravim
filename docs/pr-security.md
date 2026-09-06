# Pull request security checks

The `PR security and owner review` workflow inspects PRs targeting `develop` and
`main`. It reads contributor files as data; it never checks out a PR working tree,
initializes its submodules, installs its dependencies, or executes its build scripts.
The workflow and Python policy come from `main`. Downstream jobs use that exact
policy commit. Only GitHub-hosted runners are used.

## Checks

- Gitleaks scans the complete bounded PR diff and introduced commit patches,
  including secrets added and later removed. The trusted configuration disables
  contributor allow comments and ignore files. Findings and scanner output are
  not printed, to avoid exposing credentials. One exact public Telegram CI API
  hash is allowlisted after comparison with the official upstream commit recorded
  in `gitleaks.toml`; other API hashes remain checked.
- Actionlint validates changed active workflows. Zizmor checks their security with
  contributor configuration and inline suppressions disabled. Its generic
  `dangerous-triggers` finding is accepted only for this policy's exact
  `.github/workflows/pr-security.yml` path: `pull_request_target` is intentional
  here, and all executable code comes from the trusted default branch. Other
  findings and newly introduced privileged workflows still fail the check.
- `git diff --check` detects whitespace errors and introduced conflict markers.
- CodeQL runs C/C++ `security-and-quality` analysis with `build-mode: none` on an
  export containing only regular source files. For changes confined to translation
  units (.c/.cpp/.mm), the export includes those files and recursively resolved
  repository includes; interprocedural behavior in other translation units is not
  covered by that run. Header/build/dependency changes retain full-tree analysis.
  Macro-generated or missing external includes still limit no-build extraction.
  Warnings and errors in changed
  files fail the gate; findings in untouched files remain in the SARIF artifact.
  This is source analysis, not a build or runtime test. Generated sources,
  submodule contents, bundled third-party code, Objective-C-specific behavior,
  and dependency CVEs are not comprehensively covered. Submodule and dependency
  control changes require owner review. PRs confined to documentation and CI
  configuration skip C/C++ analysis only when no C/C++ source changes; their
  secret/workflow checks and applicable owner approval remain required.

No paid AI service, API key, or model review is used. Checks run on standard
GitHub-hosted Linux runners for this public repository. The PR comment contains
scanner results and the owner-review requirement.

## Mandatory owner review

The required status is **Telegravim / owner network review**. When review is
needed, the PR comment names **@Informativus**, lists the files and reasons, and
the workflow waits at **REQUIRED — Informativus must review network or custom code**.
After reading the changes, open the run and select **Review deployments →
network-review → Approve**. Rejection keeps merging blocked. This also supports
the owner's own PRs; GitHub does not permit approving one's own PR as a PR review.

The `network-review` environment must have exactly one required reviewer,
`Informativus`, with self-review allowed and administrator bypass disabled.
The policy checks those settings before accepting the environment as a gate.
A missing or misconfigured environment fails closed instead of silently creating
an unprotected approval step.

The default is conservative:

1. Workflow, policy, Git attribute and submodule control changes always need review.
2. A regular file is recognized as official Telegram only when its **path, Git blob
   and mode** exactly match the `dev` commit fetched directly from
   `telegramdesktop/tdesktop`. The immutable comparison SHA is shown in the PR.
   PR labels, branch names, descriptions, domains and contributor-supplied
   upstream remotes are never evidence of provenance.
3. Non-executable Markdown/text under `docs/` and a narrow set of top-level
   documentation files do not require network review.
4. Other changes require owner review. Network APIs, changed URLs and existing network
   API context produce an explicit network-related reason. Unknown code, resources,
   scripts and binary changes say that network effects **cannot be excluded**.
   This deliberately includes more than proven networking changes: textual
   matching cannot prove that an arbitrary helper has no network effects.

New commits require a new approval. The final status checks the live PR head,
base and policy SHA again. The base SHA is read from the live branch API because
GitHub may retain an older base SHA in the PR object. Branch protection must require an up-to-date branch;
otherwise a green check could survive a later base advance. Cancellation or
resource limits never produce success; a cancelled current run may leave a
pending status and must be rerun. Changing a PR base also starts a fresh run.

The official `dev` ref is resolved on every inspection, so newer upstream imports
can be recognized without letting the PR choose an allowlist. An older official
file no longer present at that tip may conservatively require review. Changing
this provenance policy itself requires owner review.

Inspection limits are recorded in `.github/security/policy.json`. At most 1,200
changed files, 250 introduced commits, 2 MB per changed blob and 8 MB per diff or
history patch set are accepted. Source export is limited to 20,000 files/150 MB.
Exceeding a bound fails inspection; it does not silently approve a partial scan.
Split oversized PRs or deliberately revise the trusted policy through review.

## Initial activation

Repository Actions were disabled before this change. The files alone do not
activate checks or protect merging. Bootstrap once, in this order:

1. Merge the task PR into `develop`, then a `develop` → `main` PR with a merge
   commit. This does not publish a release. Both branches must contain the new
   workflow, trusted scripts, and the upstream workflow archive.
2. In Settings → Environments, create `network-review` with the exact protection
   above. Leave deployment branch restrictions unrestricted: the workflow runs
   on the protected PR base, and reviewer protection supplies the authorization.
3. Enable GitHub Actions. `.github/upstream-workflows/` is an inert archive, not
   an Actions directory; the old upstream builds, publishers and issue bots must
   stay there unless individually adapted and reviewed.
4. Open a small PR and run the workflow. Verify the CodeQL result and a blocked
   owner-review environment for a custom network change.
   Only Informativus should be able to approve it. Push another commit and verify
   that approval is requested again. A documentation-only PR should require no
   network approval. Fork PRs must also be tested as read-only source inputs.
5. Add these **required status checks** to both protected branches, retain all
   existing protections, enable **Require branches to be up to date before
   merging**, and keep administrator enforcement enabled:

   - `Telegravim / security`
   - `Telegravim / owner network review`

   Select **GitHub Actions** as the expected source after observing a real run.
   These are commit statuses, not the similarly named job/check-run results.
   A PR job with a matching name does not substitute for the trusted commit status.

GitHub administrators and collaborators permitted to change protected policy,
Actions settings or write commit statuses remain trusted. GitHub cannot make a
repository administrator technically incapable of changing their own rules.
Do not grant contributor workflows write tokens or secrets, and do not add
self-hosted runners for untrusted code.

If a run is cancelled or the base/policy moves, rerun from Actions →
PR security and owner review → Run workflow, choosing `main` and the PR number.
Do not remove a required status to clear a stuck run.

## Local verification

```sh
SCANNER_BIN=/tmp/telegravim-pr-scanners python3 .github/security/install_tools.py
SCANNER_BIN=/tmp/telegravim-pr-scanners \
  python3 -m unittest discover -s .github/security -p 'test_*.py'
git diff --check
```

To reproduce a blocked secret scan from a contributor checkout, use the trusted
configuration from the protected branch and keep redaction enabled:

```sh
git fetch origin develop
# Use origin/main instead when the PR targets main.
git log --format= -p --no-ext-diff --no-textconv --diff-merges=first-parent \
  origin/develop..HEAD -- | /tmp/telegravim-pr-scanners/gitleaks stdin \
  --redact=100 --ignore-gitleaks-allow --gitleaks-ignore-path /dev/null \
  --config /path/to/trusted-checkout/.github/security/gitleaks.toml
/tmp/telegravim-pr-scanners/actionlint .github/workflows/*.yml
/tmp/telegravim-pr-scanners/zizmor --offline --no-config --no-ignores \
  --strict-collection .github/workflows/*.yml
```

Zizmor's `dangerous-triggers` warning for `pr-security.yml` is the one documented
exception; other findings must be resolved. Never paste raw secret findings into
PR comments. For CodeQL, download the `codeql-findings` SARIF artifact from the
failed run; the artifact retains locations and rule identifiers.

Scanner versions and archive SHA-256 values are fixed in `install_tools.py`.
Dependabot maintains GitHub Action SHA pins; scanner release updates require a
reviewed policy change. Security boundary tests cover provenance, unknown code,
paths, stale approvals, scanner suppression and removed secrets.

## Design decisions

The implementation uses deterministic checks without a paid AI service.
Network-only textual classification was rejected because missed helper calls
could waive a mandatory review. Blanket CODEOWNERS approval was rejected because
it cannot cover arbitrary new files reliably and cannot approve the owner's own PRs.
A protected environment gives an explicit owner action without parsing mutable
approval comments. The conservative unknown-code rule favors missed-risk
prevention over minimizing the number of manual reviews.
