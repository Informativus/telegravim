# Telegram Vim Keymap Fork Tasks

This folder tracks the staged work for a maintainable Telegram Desktop fork
with a full Vim-style keyboard layer and a practical update path for macOS.

These are historical planning notes. Their `vim-keymap` branch and upstream
rebase instructions are superseded by [the current branch workflow](../../../docs/branch-flow.md):
task branches -> PR to `develop` (betas) -> release PR to `main` (stable).
Use an upstream integration branch and PR instead of rebasing shared branches.

The default direction is:

1. Keep the fork easy to rebase on `upstream/dev`.
2. Build the Vim keymap as a narrow, isolated feature.
3. Add update notification before attempting full self-update.
4. Treat in-app self-update as a separate release-infrastructure project.

## Stage Index

- [00-decisions.md](00-decisions.md) - product and technical decisions to lock first.
- [01-fork-maintenance.md](01-fork-maintenance.md) - make the fork safe to keep updated.
- [02-vim-keymap-mvp.md](02-vim-keymap-mvp.md) - first usable Vim navigation layer.
- [03-vim-keymap-full.md](03-vim-keymap-full.md) - expand toward a complete keymap.
- [04-macos-build-signing.md](04-macos-build-signing.md) - local macOS build, signing, and install path.
- [05-update-notifications.md](05-update-notifications.md) - notify when a new fork build is available.
- [06-self-updater.md](06-self-updater.md) - optional full in-app auto-update.

## Operating Rules

- Keep upstream changes and fork changes clearly separated.
- Do not commit secrets: Telegram `api_id`, `api_hash`, Apple credentials, or update signing private keys.
- Do not change the official Telegram updater endpoint until a fork update endpoint exists.
- Prefer Debug builds for feature verification. Release/notarized builds are a release task.
- Keep changes narrow enough that rebasing on Telegram releases is routine, not a rewrite.
