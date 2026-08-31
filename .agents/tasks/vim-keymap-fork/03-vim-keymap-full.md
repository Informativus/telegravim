# Stage 3: Full Vim Keymap

## Goal

Expand the MVP into a fuller Vim-inspired command layer for Telegram workflows.

## Tasks

- Add a command dispatcher that supports multi-key sequences.
- Add timeout/reset behavior for partial sequences.
- Add a visual hint or minimal status indicator only if needed.
- Add navigation commands:
  - `gg`: top / first chat.
  - `G`: bottom / last chat.
  - `Ctrl-d` / `Ctrl-u`: half-page scroll.
  - `Ctrl-f` / `Ctrl-b`: page scroll.
  - `n` / `N`: next/previous search result where applicable.
- Add chat actions:
  - open selected chat;
  - archive chat;
  - mark read/unread;
  - open context menu;
  - reply to selected message if selection exists.
- Add mode transitions:
  - `i`: focus compose box.
  - `a`: focus compose box at end.
  - `/`: search.
  - `Esc`: return to normal mode.
- Add conflict handling:
  - do not steal keys from active text input;
  - do not break media viewer shortcuts;
  - do not break modal/dialog keyboard behavior.
- Add a small internal test strategy if the touched behavior becomes broad.

## Non-Goals

- No self-updater.
- No custom plugin system.
- No scripting language for keymaps unless a real need appears.

## Acceptance Criteria

- Common Telegram workflows can be done without mouse:
  - move between chats;
  - open chat;
  - search;
  - type and send;
  - return to normal navigation;
  - archive/read/menu actions.
- Partial sequences reset predictably.
- Text entry remains reliable.
- Conflicts are documented.

## Verification

- Manual end-to-end keyboard session.
- Debug build if dependencies exist.
- Focused tests only for parser/dispatcher behavior if implemented as durable logic.

## Output Artifact

- A coherent Vim keymap layer with documented commands.
