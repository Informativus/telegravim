# Automatic updates

Telegravim Desktop and the standalone Televim Android application use the native
Telegram update controls with a project-owned update source. The published targets
are macOS ARM64, Windows x64, Linux x86-64, and a universal Android APK (API 26+).
Store-managed installations retain their platform update mechanism.

## Distribution and trust

Every client requests the same static HTTPS resource:
`https://informativus.github.io/telegravim/updates/current6`. The request contains
no account, contacts, session, imported settings, installation identifier or
telemetry. A hosting provider necessarily receives the connection IP address and
ordinary HTTP/TLS transport information. The updater does not use Telegram's
account API, Telegram update channels or a server-provided update prefix.
Android import activities do not construct or start the updater; automatic checks
run from the existing normal application lifecycle after initialization. Android
notifications appear in the application; this does not add background push
notifications while the application is closed.

The resource contains base64-encoded `payload` and `signature` fields. The
signature is ES256 with a 64-byte raw `r || s` representation, over the exact bytes
`televim-catalog-v1\n` followed by the decoded payload. The catalog contains its
format, issue and expiry times, release source commit, native platform versions
and immutable GitHub Release asset URLs. Checks reject malformed, expired and
untrusted catalogs. Request sizes, redirects and timeouts are bounded. Offline
or signature failures remain failures; a manual check must not report that the
installed version is current when discovery failed.

Desktop packages use the existing TDUP v2 envelope and native Packer/Updater.
The fork's Ed25519 root authenticates the delegated ES256 release key manifest.
Clients verify channel, operating system, architecture, increasing native version
and payload signature before decompression. The downloaded bytes must also match
the signed catalog size, SHA-256 and exact native version before extraction;
catalog verification honors persisted root-authorized key revocations. Legacy RSA
updates are rejected.
The four native channel entries remain in the trust manifest; this publication
workflow publishes only stable packages. Existing beta opt-in falls back to the
stable feed. Public/private canary releases are not published by this workflow.

Android supports ES256 on the minimum API level without new JNI crypto. Before
installation it rechecks the signed SHA-256 and size, package identity, exact
announced version, increasing installed version and the installed signing
certificate. APK files stay in the application's private files directory until
shared read-only with the system installer. Downloaded packages are verified
again after restart and immediately before installation. Android permission and
installer confirmation screens remain mandatory where the OS requires them.

## Release procedure

1. Increase `AppVersion` and its display strings in `Telegram/build/version` and
   `core/version.h`, and increase `APP_VERSION_CODE` in the Android overlay.
   Android's standalone ABI suffix is `9`, so base `7239` produces APK code
   `72399`. Date tags and the product display label are not update eligibility.
2. Merge the task PR into `develop`, then merge the reviewed `develop` release PR
   into `main` using a merge commit. Create a new immutable stable tag there and
   a draft GitHub Release with the release notes. Do not reuse a failed tag.
3. Dispatch the macOS, Windows, Linux and Android package workflows on that exact
   tag, using the universal Android ABI option. All four workflow revisions and
   artifact source receipts must match the tag commit. Review their protected
   environment gates. Each Desktop workflow includes its native Updater and an
   unsigned native envelope, alongside the bootstrap installer.
4. Dispatch **Publish verified automatic updates** on that same tag. Supply the
   four successful run IDs. Use `bootstrap` only for the initial HTTP-404 catalog.
   The existing `network-review` gate protects the signing credential and release
   publication. The workflow verifies source, platform, versions and artifact
   hashes, signs envelopes with the native Packer's final verification, uploads
   the full set to the draft, verifies GitHub asset digests, then publishes.
5. GitHub Pages deploys the complete signed catalog atomically. Clients continue
   using the preceding catalog until this deployment completes. If deployment
   fails after release publication, rerun with the same builds: existing assets
   must match exactly and are never replaced. A newer catalog cannot be replaced
   by a release with lower or reused native versions.

If publication tooling needs a repair after client builds have been frozen,
merge that repair through the normal task and release PRs before dispatching
the publisher from the reviewed `main` revision. Keep the original client tag
and all four build run IDs. Both the workflow and script require the tag to be
an ancestor of the publisher and the publisher to be an ancestor of `main`.
The complete tree difference is restricted to `Telegram/build/update_release.py`,
`.github/workflows/updates.yml` and this document. Client source, package workflows,
versions, native Packer and trust files must remain identical. The executing
workflow and checkout must have the same commit, with a clean tracked tree.
Draft releases are resolved to a database ID, which remains fixed through upload
verification; the tag and stable-release identity are checked again before publication.

Pages must use the GitHub Actions build type. The `network-review` environment
holds `TELEVIM_UPDATE_SIGNING_KEY` (PEM ES256 private key). It must never appear in
source or artifacts. The root private key remains offline and is not a CI secret.
The public keys and root-signed manifest are in `Telegram/Resources/update/`.
The Android catalog key is embedded in `UpdateController`; rotation requires a
client release signed by the previously trusted key. Desktop envelope delegation
can also be rotated/revoked using a newer root-signed manifest. Loss or compromise
of the catalog signing key requires a reviewed bootstrap/recovery release;
changing a feed URL is not a trust reset.

Catalogs expire after 365 days. The publication workflow's `refresh` operation
verifies the existing signed catalog and every referenced published package's
GitHub digest, then renews the catalog without replacing any release assets.
Run it from `main` before expiry if no newer version has shipped; it does not
need retained Actions build artifacts. It uses the same owner approval gate.
Failure leaves the installed application usable and the previous release assets
available. No automatic rollback of user data is made.

## Design decisions

- Static Pages plus immutable Releases avoids a separate server and account API.
  A dedicated VPS was rejected as unnecessary operational work; unsigned release
  API discovery was rejected because it does not authenticate release policy.
- Native Desktop envelopes and Android's native custom-update hook preserve the
  existing notification, progress and install/restart UI. Shipping only DMG/ZIP
  links would not implement native Desktop installation.
- A separate root and release key keep root material out of CI. ES256 avoids an
  Android API-33-only Ed25519 provider and new native crypto code.
- Publication requires every shipped platform at one source revision. This
  prevents users from seeing a release whose matching clients are incomplete.
- Old clients shipped with updates disabled. They require one manual bootstrap
  installation; a server cannot enable code absent from an installed client.

## Verification boundaries

Record source review, existing checks, each OS build, signed package integrity,
Pages deployment, and actual client update/install observation separately in the
release evidence. A passing package signature check is not OS installation proof.
The full native update verifier is available as `test_update_verify`; no new test
sources are required for this change. Authenticated sessions and accepted product
features must survive the bootstrap and subsequent update.
