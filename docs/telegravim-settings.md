# Telegravim settings and read receipts

Settings → Telegravim groups Vim and keyboard options, free local features, and privacy. Existing native editors and Ctrl+V previews remain available.

The account-scoped privacy switch is off by default. When enabled, opening a normal full-size chat, scrolling, and playing voice or video messages do not acknowledge reading or listening. A successfully sent new message acknowledges that conversation through its confirmed server message ID. Failed or queued sends, edits, reactions, scheduled messages and template editing do not unlock acknowledgements. Incoming messages newer than the confirmed send remain unread.

Each forum topic or channel conversation has its own boundary. Played media acknowledgements are deferred until a successful send in the same conversation. Server updates from other devices remain authoritative; this switch affects only this client. Requests already sent before enabling cannot be undone. Turning the switch off restores normal Telegram reading behavior.

## Decisions

- Use one persistent account preference and normal chat windows, rather than adding a second full-screen preview implementation. This matches the confirmed user request and preserves native navigation.
- Keep the small Ctrl+V preview unchanged.
- Keep the existing native local-feature flows and the separate free and Premium feature groups.
- Gate reading before local unread mutations and again at delayed request dispatch. Use server confirmation rather than pressing Send as the release boundary.
- Keep the preference in account key-value storage; do not change positional binary serialization. Deferred acknowledgements stay in memory and are discarded on restart.
- Use existing native checks, compilation, source review and user acceptance. No new tests are authored for this task.
