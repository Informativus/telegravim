# Stage 1: Fork Maintenance Foundation

## Goal

Make the repository safe to update from Telegram upstream while keeping fork
changes isolated and understandable.

## Tasks

- Add `origin` remote pointing to the personal GitHub fork.
- Keep `upstream` pointing to `https://github.com/telegramdesktop/tdesktop.git`.
- Document the update command sequence:
  - `git fetch upstream --tags --recurse-submodules`
  - `git rebase upstream/dev`
  - `git submodule update --init --recursive`
- Create a small fork maintenance note with:
  - current upstream SHA;
  - current fork branch;
  - known local patches;
  - manual conflict-resolution notes.
- Identify files likely to conflict during rebases:
  - `Telegram/SourceFiles/core/shortcuts.*`
  - keyboard event handlers under `history/` and `dialogs/`
  - macOS build/update scripts if later changed.
- Add a lightweight rebase checklist.

## Non-Goals

- No product behavior change.
- No update server.
- No release build.

## Acceptance Criteria

- `origin` and `upstream` remotes are configured.
- `vim-keymap` branch pushes to `origin`.
- The repo can fetch upstream and report a clean working tree.
- A rebase checklist exists in this task folder.

## Verification

```bash
git remote -v
git status --short --branch --untracked-files=no
git fetch upstream --tags --recurse-submodules
git log --oneline --decorate -5
```

## Output Artifact

- A maintainable fork branch with documented update commands.
