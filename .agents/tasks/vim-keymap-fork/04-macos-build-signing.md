# Stage 4: macOS Build, Signing, And Installation

## Goal

Produce a macOS `.app` that can be installed and launched reliably on the local
machine, then prepare the path toward signed/notarized distribution.

## Tasks

- Prepare Telegram Desktop macOS dependencies using upstream instructions.
- Provide Telegram `api_id` and `api_hash` at configure time without committing
  them.
- Build Debug first.
- Build a local app bundle.
- Decide signing mode:
  - ad-hoc signing for local testing;
  - Developer ID signing for distribution outside the Mac App Store.
- If distributing outside the local machine:
  - configure Apple Developer ID Application certificate;
  - sign the app with hardened runtime;
  - notarize release artifact;
  - staple notarization ticket.
- Decide app name and bundle identity before release signing.
- Verify whether the fork replaces `Telegram.app` or installs next to it.

## Likely Files

- `docs/building-mac.md`
- `Telegram/build/build.sh`
- macOS entitlements under `Telegram/Telegram/`
- generated app bundle metadata.

## Non-Goals

- No self-update feed.
- No release automation until manual build is proven.
- No secrets in the repository.

## Acceptance Criteria

- Debug app launches locally.
- App can log into Telegram using valid API credentials.
- Local install path is documented.
- Signing/notarization requirements are documented before public distribution.
- Existing official Telegram install is not damaged accidentally.

## Verification

```bash
codesign --verify --deep --strict /path/to/Telegram.app
spctl --assess --type execute /path/to/Telegram.app
```

Use these only against the fork build path, not an installed official Telegram
client.

## Output Artifact

- A locally installable macOS fork build and a documented signing path.
