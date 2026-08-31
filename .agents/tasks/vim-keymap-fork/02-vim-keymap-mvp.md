# Stage 2: Vim Keymap MVP

## Goal

Build the smallest real Vim-style navigation layer that is useful daily and
does not make normal message typing unreliable.

## Concept

Telegram's existing shortcut system maps single `QKeySequence` bindings to
commands. A full Vim keymap needs more than that: modes, multi-key sequences
such as `gg`, text-input awareness, and context-specific actions.

So the MVP should introduce a separate Vim keymap layer instead of forcing all
behavior into `shortcuts-custom.json`.

## Tasks

- Add a narrow Vim keymap module, for example under `Telegram/SourceFiles/core/`.
- Define modes:
  - Normal mode.
  - Insert/text mode passthrough.
- Ensure text fields keep normal typing behavior.
- Add first navigation bindings:
  - `j` / `k`: move chat selection or scroll messages depending on focus.
  - `h` / `l`: previous/next folder or back/forward where safe.
  - `/`: focus search.
  - `Esc`: leave text/search mode or clear transient UI.
- Add Russian-layout equivalents for core movement if needed:
  - `о` / `л` for `j` / `k` where the previous config already used them.
- Add a setting or feature flag so Vim mode can be disabled.
- Keep all direct patches into existing Telegram files minimal.

## Likely Files

- `Telegram/SourceFiles/core/shortcuts.h`
- `Telegram/SourceFiles/core/shortcuts.cpp`
- `Telegram/SourceFiles/history/history_widget.cpp`
- `Telegram/SourceFiles/history/view/history_view_list_widget.cpp`
- `Telegram/SourceFiles/dialogs/dialogs_widget.cpp`
- `Telegram/SourceFiles/dialogs/dialogs_inner_widget.cpp`
- settings UI only if the feature flag is exposed in app settings.

## Non-Goals

- No full command grammar.
- No update notification.
- No release packaging.
- No broad refactor of Telegram input handling.

## Acceptance Criteria

- With Vim mode enabled, `j/k` navigates in the main Telegram UI without typing
  letters into the compose box unless the compose box is intentionally focused.
- With Vim mode disabled, Telegram behaves like upstream.
- Existing shortcuts still work.
- Russian keyboard equivalents are preserved for agreed core bindings.
- The implementation can be rebased with a small, understandable conflict set.

## Verification

- Static search confirms the feature is isolated to a small number of files.
- Build Debug target if local dependencies are available.
- Manual run verifies:
  - chat list navigation;
  - message list scrolling;
  - search focus;
  - normal message typing;
  - escape behavior.

## Output Artifact

- A daily-usable Vim navigation MVP.
