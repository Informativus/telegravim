# Contributing to Telegravim

Telegravim is an independent Telegram Desktop fork. Submit contributions to
[Informativus/telegravim](https://github.com/Informativus/telegravim/pulls).
The [branch and release workflow](../docs/branch-flow.md) is the source of truth
for branch names, pull requests, beta builds, stable releases, and upstream
integration.

## Branches and pull requests

- `main` is the default branch and contains stable releases.
- `develop` contains beta versions and ongoing integration.
- Start each task on a separate branch from current `origin/develop`, using
  `feature/*`, `fix/*`, `docs/*`, or `chore/*`, and open a PR into `develop`.
- Promote a release through a `develop` -> `main` PR with a merge commit.
- All changes require PRs, including documentation, dependencies, and bot or
  agent changes. Never push directly to either shared branch or force-push it.
- Use merge commits when merging PRs. Do not squash or rebase the permanent
  branches, and do not bypass branch protection.
- Stable release tags come from the merged `main` commit. Beta tags come from
  `develop`, and beta GitHub Releases must be marked as prereleases.

## Review and verification

Keep each PR focused on one task. Explain the resulting behavior and report
what was tested, including any remaining verification limits. Do not mix
unrelated formatting changes into a fix. Follow [REVIEW.md](../REVIEW.md) for
code style and [AGENTS.md](../AGENTS.md) for repository requirements and commit
messages. Resolve review discussions before merging.

Use verification proportional to the change. Documentation changes do not
require an application build. Product changes need relevant build and behavior
checks; high-impact behavior needs regression coverage as described in
`AGENTS.md`. See the [build instructions](../README.md#build-instructions).

## Updating from Telegram Desktop

Fetch from `upstream`, integrate the selected upstream commit on a separate
`chore/*` branch based on `origin/develop`, resolve conflicts, update submodules,
and verify the result. Submit that branch as a PR into `develop`.
Never rebase or force-push `main` or `develop` onto upstream. Follow the
[upstream integration commands](../docs/branch-flow.md#обновление-telegram-upstream).
