# Stage 5: Update Notifications

## Goal

Notify from inside the fork when a newer fork build is available, without
attempting automatic replacement of the app bundle.

## Why This Stage Comes Before Self-Update

Notification-only updates are much simpler and safer. They prove the release
flow, version comparison, and user messaging before the app starts modifying
itself on disk.

## Tasks

- Define a small release manifest format, for example:
  - fork version;
  - upstream Telegram version;
  - upstream commit SHA;
  - release URL;
  - changelog URL;
  - minimum supported macOS version;
  - architecture entries for `mac` and `armac`.
- Host the manifest:
  - GitHub Releases asset;
  - GitHub Pages;
  - or any static HTTPS endpoint.
- Add a fork-specific update checker that reads only our manifest.
- Compare current fork version against manifest version.
- Show a clear in-app notification:
  - "New Telegram Vim build available";
  - upstream Telegram version included;
  - release notes link;
  - download/install action.
- Keep the official Telegram updater endpoint untouched unless full self-update
  work begins.
- Add a manual release checklist:
  - rebase upstream;
  - build;
  - sign/notarize if applicable;
  - upload artifact;
  - update manifest;
  - verify app sees the new manifest.

## Non-Goals

- No automatic install.
- No RSA update-pack.
- No mutation of `tupdates`.
- No app bundle replacement.

## Acceptance Criteria

- App can detect a newer fork release.
- App does not offer official Telegram builds as fork updates.
- Notification opens the correct fork release page.
- If the manifest is unavailable, Telegram continues working normally.

## Verification

- Test with a local or staging manifest that advertises a higher version.
- Test with same/lower version.
- Test malformed/unreachable manifest.
- Verify no official Telegram updater files are downloaded by this feature.

## Output Artifact

- Safe in-app update notification for fork releases.
