# Stage 0: Decisions

## Goal

Lock the choices that affect every later stage, so we do not build a fork that
is hard to install, hard to update, or hard to rebase.

## Tasks

- Decide app identity:
  - Option A: replace official `Telegram.app` for personal use.
  - Option B: ship a separate app, for example `Telegram Vim.app`.
- Decide data profile:
  - Reuse existing Telegram Desktop data.
  - Use a separate data folder for the fork.
- Decide update ambition:
  - Notification-only first.
  - Full self-update later.
- Decide whether the fork is private personal use or shared with other users.
- Choose a version naming scheme, for example `7.1.1-vim.1`.
- Choose where release artifacts will live:
  - GitHub Releases.
  - A small static HTTPS host.
  - Both.

## Recommended Defaults

- Use a separate branch: `vim-keymap`.
- Start with notification-only updates.
- Keep release artifacts in GitHub Releases initially.
- Keep self-update optional until the app can be signed and notarized reliably.

## Non-Goals

- No Vim implementation here.
- No updater implementation here.
- No macOS signing setup here.

## Acceptance Criteria

- App identity choice is written down.
- Data profile choice is written down.
- Update strategy for the first usable release is written down.
- Release artifact location is chosen.

## Output Artifact

- A short decision section added to this file or a follow-up `decisions.local.md`.
