# Android client and offline settings transfer

## Scope

The Android client lives in `android/` in this repository. Its starting point is
the official Telegram Android submodule at
`3de37c8cfb039452be7398909461da6288babf15` (13.1.1). Android dependencies retain
their pinned upstream revisions. Televim code and build configuration are
reviewable overlays and patches in `android/`, with content hashes in
`android/source.json`. A deterministic preparation step checks clean dependency
pins and creates the source tree inside ignored `.local/`; it never modifies
the dependency checkout. Source metadata binds the monorepo revision, dependency
revisions and overlay content. Desktop and Android changes follow the same
task branch, pull request, integration and release process.

A signed Android APK belongs in the same GitHub Release as the Desktop assets.
An APK and its checksum must identify the exact source revision and signing
identity. Android has a stable application ID and a separate signing key.
Upstream demonstration credentials and signing files must not be used for a
Televim release. App API credentials and signing material are private build
inputs, not source files.

The initial importer applies two application-wide preferences:

- Send messages with Enter.
- Display large emoji.

Other transferred modules remain stored without being executed. This is not
Android support for local folders, Saved Messages tags, local pins or every
other Desktop feature. The preview and result distinguish settings actually
applied from data retained for compatibility. An import with no applicable
preferences must not report that settings were applied.

The portable settings schema is documented separately in
[`portable-preferences.md`](../local-features/portable-preferences.md).

## Offline boundary

Export, link decoding, QR scanning, package validation, preview and application
must not send network requests. This includes LAN discovery, fetching a package
from a URL, remote QR recognition, analytics and Telegram API fallback.
Normal Telegram messaging remains a networked feature of the client.

The importer is a standalone framework Activity in the main application process.
Opening its link or local-file route does not construct `LaunchActivity`, the
native language controller or account controllers. A no-display Activity routes
ordinary sharing to the native client and recognized settings to the importer,
preserving read grants and `ClipData`. Normal launcher icons and Telegram links
retain their native entrypoint. QR decoding stays within the importer Activity.

Push/proxy startup, ML Kit and EmojiCompat initialization wait for normal native
application initialization. Configuration changes before that point do not
instantiate native language or UI controllers. Firebase keeps its original
provider ordering, with messaging auto-init and data collection disabled. The
release has no default Firebase configuration; adding one requires reviewing this
startup boundary again. Final APK inspection must check these conditions and
that the framework import strings survive release resource shrinking.

An enabled local Televim passcode protects the importer through the native
checker and retry limit. This unlocks only import, without unlocking chats. The
export password is requested separately afterward. Leaving the screen or a
passcode change invalidates the preview and pending callbacks. Cancelled key
derivation retains the process-wide worker slot until it actually finishes.
Existing messaging services can continue independently; source isolation does
not replace traffic observations on a device.

The link contains the complete encrypted package:

```text
televim://settings/v1#<unpadded-base64url-of-the-entire-tgv-envelope>
```

There is no package server, callback URL, device connection address, embedded
password or decryption key. Base64url is only an encoding. The user enters the
export password separately on the receiving device. It is not their Telegram
password. Passwords, payloads and complete import links must not enter logs.

The wire encryption is the existing
[`.tgv` version 1 format](../local-features/file-transfer-format.md):
PBKDF2-HMAC-SHA256 with the fixed versioned parameters, AES-256-GCM, and an
authenticated manifest and package stream. Password encoding is exact UTF-8
without normalization. Unknown cipher, KDF or format parameters are rejected.
The importer authenticates the bounded package before interpreting its JSON.

The complete ASCII URI is limited to 8192 bytes. A single QR is limited to
2900 URI bytes at Low error correction, and generation must still check the
actual encoder result. No stage may truncate an oversized link. Larger exports
use an encrypted local file. The QR screen explains the separate password and
offers the file route before attempting an oversized QR.

The Android QR decoder is bundled with the app and uses camera frames locally.
It must not download a recognition model or pass frames to another service.

## Local files

For an already-local JSON file in Android's file manager, use **Open with**
and select Televim. Encrypted `.tgv` files support both **Open with** and
**Share** to Televim. Sharing an ordinary JSON file retains Telegram's native
file-sending behavior; an explicit `application/x-televim-settings` MIME type
still selects the importer. The app does not promise a universal offline system
file picker: a document provider or picker can contact a cloud service even
when a URI starts with `content://` or `EXTRA_LOCAL_ONLY` is requested.

Before provider queries or file reads, verify the granted read access and the
supported local authority, package and system-provider identity. Reject remote,
virtual and unsupported providers and all HTTP, HTTPS, nested intent and URL
fallbacks. A rejected source must explain the supported local route. Only file
manager/provider combinations actually observed during acceptance can be
claimed as verified.

Import accepts a single file. Sharing an ordinary JSON file opens normal chat
sending and can initialize the networked client; use **Open with** for import.

Initial Android bounds are 8 MiB for an encrypted input, 4 MiB aggregate JSON,
2 MiB for an individual module or manifest, 32 JSON nesting levels and 10000
records. Check bounds before allocating or decoding larger buffers. Desktop's
larger full-data archives can exceed these receiver limits; reject them with a
clear size error, without partially applying the package.

Validate UTF-8, duplicate JSON keys, versions, account identities, manifest
entries, canonical paths, lengths, SHA-256 values and the exact end of the
package. Unknown modules and fields are data, not commands, paths to open or
URLs to fetch. Raw JSON accepts only the documented settings schema.

## State publication

Private E2E identity keys, Telegram authorization keys and session material
remain outside the transferable module registry. An import never authorizes an
account or activates an automation executor. Profiles use stable account IDs;
the two global preferences always target the application's `mainconfig` file,
not whichever account profile happens to be visited last. Conflicting global
values require an explicit decision.

Retained packages and generation metadata are encrypted using an Android
Keystore key and kept under `noBackupFilesDir`. Retention is bounded. Existing
archives are not silently evicted. The original encrypted package is retained
without its password so unsupported contents can be recovered faithfully.
Disable application backup and the upstream custom backup agent for this fork.

Before publication, write and flush an immutable generation and its directory
entry. One synchronous `mainconfig` editor commit publishes both the generation
pointer and the supported native preferences. No controller or Telegram API is
needed to access this local preference file.

Apply runs serially on the UI thread with the relevant native setters. Recheck
the generation and the presence and values of every affected native key against
the preview. A stale preview must not overwrite a later user change. Refresh
native cached values and existing compose views before reporting success.

An ambiguous failed commit is not success and is not proof that nothing changed.
Restore the previous pointer and key presence/values as one serialized editor
operation, retain both generations and prevent a blind retry until restart and
reconciliation. A cached SharedPreferences read is not disk verification. A
missing or corrupt referenced generation produces a local error; never silently
reset the pointer or execute partially recovered data.

## Decisions and review

The requirements were explicitly confirmed before design. Technical choices
were delegated to the implementing agent.

| Decision | Alternatives considered | Resolution |
| --- | --- | --- |
| Pinned Android submodule with monorepo overlays and patches | Raw source vendoring; port Desktop Qt; maintain an external Televim fork | Preserve native Android behavior, reproducible source provenance and the existing trusted PR size limits |
| Existing encrypted `.tgv` envelope | Plain Base64 JSON; QR pairing over LAN | Reuse the file contract while keeping transport entirely offline |
| Explicit receiver size bounds | Claim streaming solely because reads use small buffers | Bound possible JCA GCM buffering and JSON allocation |
| One preference-file publication point | Independent writes to module state and native preferences | Publish the generation pointer and native settings together |
| Explicit incoming local file routes | Assume any SAF URI or system picker is offline | Validate supported providers before access; do not promise unverified picker behavior |
| Preserve unsupported modules without execution | Discard them; silently claim Android feature parity | Show exactly what was applied and what remains inactive |

The sequential Skeptic, Constraint Guardian and User Advocate reviews identified
GCM buffering, startup ordering, multi-store atomicity, preference cache refresh,
SAF networking, password encoding, QR capacity and misleading success messages.
These objections were accepted and incorporated above. The independent Arbiter
approved the design for implementation. Design approval is not runtime or
release acceptance.

## Acceptance and delivery

Use existing checks, compilation, builds and focused observation of the actual
application. No new tests, fixtures or test-only helpers are authorized for this
task. Preserve existing upstream tests.

Required evidence includes:

- A native Android build, APK signature verification and matching SHA-256.
- Desktop export and Android import with ASCII, Cyrillic and supplementary
  Unicode passwords, including a wrong-password rejection.
- A real phone scanning the QR displayed by Desktop, including a dense code.
- The documented local file entrypoints, cancellation, size rejection and
  unsupported-module preview behavior.
- Native preference changes, duplicate import behavior, stale preview rejection
  and persistence after application restart.
- Observed absence of requests attributable to import, separately from ordinary
  Telegram background connections.
- Failure/restart behavior around generation and preference publication.
- A fresh signed Release DMG if Desktop changes are delivered for macOS use.

Deliver source through a task PR into `develop`. Stable release source passes
through a `develop` to `main` merge-commit PR. Publish only verified artifacts
from the selected release revision. Do not replace assets or move existing
release tags, and do not equate a workflow artifact with a published release.
