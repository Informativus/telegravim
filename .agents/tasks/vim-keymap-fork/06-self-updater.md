# Stage 6: Full Self-Updater

## Goal

Use an in-app update flow where the fork downloads a signed fork update-pack
and replaces the installed `.app`.

This is optional and should start only after notification-only releases are
working reliably.

## Required Infrastructure

- Private RSA key for signing fork update-packs.
- Matching public RSA key embedded in the fork.
- A fork update endpoint compatible with Telegram's `/current` response.
- Hosted update-pack files:
  - `tmacupd{version}` for Intel macOS.
  - `tarmacupd{version}` for Apple Silicon macOS.
- Signed and notarized `.app` release artifacts.
- Rollback plan for a bad release.

## Tasks

- Generate fork update signing keys.
- Replace embedded update public keys with fork public keys.
- Patch the packer build so it uses fork private keys from a secret location,
  never from the repository.
- Point the fork updater to a fork-owned HTTPS endpoint.
- Disable or bypass MTP update lookup if it can still discover official
  Telegram updates.
- Confirm `Platform::AutoUpdateKey()` maps correctly:
  - `mac` for Intel;
  - `armac` for Apple Silicon or Rosetta.
- Patch macOS updater assumptions if app name is not `Telegram.app`.
- Build update-pack files with the fork packer.
- Host update-pack files and manifest/current response.
- Verify update-pack signature validation.
- Verify update install on a disposable app copy first.
- Verify update does not touch official Telegram app or unrelated data.

## Likely Files

- `Telegram/SourceFiles/config.h`
- `Telegram/SourceFiles/core/update_checker.cpp`
- `Telegram/SourceFiles/storage/localstorage.cpp`
- `Telegram/SourceFiles/_other/packer.cpp`
- `Telegram/SourceFiles/_other/updater_osx.m`
- `Telegram/build/build.sh`

## Non-Goals

- No public release until rollback is proven.
- No committed private keys.
- No update endpoint that can serve official Telegram packages to the fork.

## Acceptance Criteria

- Fork app downloads only fork update-packs.
- Update-pack signature verification uses fork public key.
- Update from version N to N+1 succeeds on a disposable install.
- A failed update leaves the existing app usable.
- Release process can reproduce update-pack generation.
- Rollback or manual reinstall instructions exist.

## Verification

- Install old fork build into a disposable path.
- Serve a staging `/current4` response with a newer fork version.
- Let the app download and unpack update.
- Quit/relaunch through updater.
- Confirm new app version, signature, and bundle path.
- Confirm official Telegram app remains untouched.

## Output Artifact

- Fork-owned in-app update pipeline.
