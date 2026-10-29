## Telegram messenger for Android

[Telegram](https://telegram.org) is a messaging app with a focus on speed and security. It’s superfast, simple and free.
This repo contains the official source code for [Telegram App for Android](https://play.google.com/store/apps/details?id=org.telegram.messenger).

## Creating your Telegram Application

We welcome all developers to use our API and source code to create applications on our platform.
There are several things we require from **all developers** for the moment.

1. [**Obtain your own api_id**](https://core.telegram.org/api/obtaining_api_id) for your application.
2. Please **do not** use the name Telegram for your app — or make sure your users understand that it is unofficial.
3. Kindly **do not** use our standard logo (white paper plane in a blue circle) as your app's logo.
3. Please study our [**security guidelines**](https://core.telegram.org/mtproto/security_guidelines) and take good care of your users' data and privacy.
4. Please remember to publish **your** code too in order to comply with the licences.

### API, Protocol documentation

Telegram API manuals: https://core.telegram.org/api

MTproto protocol manuals: https://core.telegram.org/mtproto

### Building Televim

The Android source is pinned to official Telegram commit
`3de37c8cfb039452be7398909461da6288babf15`. The official source is the `android/upstream` submodule. Televim changes live
in `overlay/` and `patches/`; `source.json` pins their content. Initialize from
the monorepo root with:

```bash
git submodule update --init --recursive --depth 1 --jobs 2 -- android/upstream
```

The preparation script checks source and nested dependency pins, applies the
reviewed changes to an ignored source tree under `.local/`, and records source
provenance. It never edits the upstream checkout. Upstream demo signing and
service configuration files are omitted from the prepared tree.
The supported app module is `TMessagesProj_AppStandalone`, with stable app ID
`org.informativus.televim` and minimum Android API 26. The original Java namespaces
remain intact for upstream compatibility.

Use JDK 17, Android SDK/platform 36, Build Tools 36.0.0, NDK 27.2.12479018 and
CMake 3.22.1. Set `JAVA_HOME` and `ANDROID_SDK_ROOT` to these installations.
Gradle 8.13 is pinned by the wrapper. The build entry point limits Gradle to
2 GiB, two workers and no parallel projects, and native Ninja to two jobs.
Generated outputs and the Gradle cache go under the primary checkout's `.local/`.

Obtain your own Telegram API application at <https://my.telegram.org/apps>.
Set `TELEVIM_API_ID` and `TELEVIM_API_HASH` in the environment, or set
`TELEVIM_CONFIG_FILE` to a private Java-properties file containing those keys.
The build accepts no embedded fallback application credentials. Never commit the
private file, keystore, passwords, generated BuildConfig or build logs.

From `android/`, build one ABI for local development:

```bash
./build.sh debug arm64-v8a
```

The debug variant uses a locally generated Android debug signing key. It cannot
serve as the release artifact. Release builds require all four private settings:
`TELEVIM_KEYSTORE`, `TELEVIM_STORE_PASSWORD`, `TELEVIM_KEY_ALIAS`, and
`TELEVIM_KEY_PASSWORD`, supplied through the same environment/private file.
The keystore must be owned by this fork. Missing signing settings stop the build.

```bash
./build.sh release arm64-v8a
./build.sh release armeabi-v7a,arm64-v8a,x86,x86_64
```

The prepared source uses
`:TMessagesProj_AppStandalone:assembleAfatStandalone` and
`-PtelevimAbis=arm64-v8a` (or the explicit comma-separated ABI list), with
`--no-daemon --no-parallel --max-workers=2`. Keep `TELEVIM_BUILD_ROOT` and
`GRADLE_USER_HOME` inside the checkout's ignored `.local/` directory.
The APK is written under
`$TELEVIM_BUILD_ROOT/TMessagesProj_AppStandalone/outputs/apk/afat/standalone/app.apk`.
Pull requests compile Android Java and resources on a GitHub-hosted runner using
invalid placeholder API credentials. This check receives no private credentials
and produces no installable artifact. The manually dispatched Android workflow
uses the protected owner-review environment to produce a verified signed build artifact.
Publishing that APK into the matching Desktop release is a separate release step.
A workflow artifact alone is not a published GitHub Release.

The fork does not use upstream Google/Huawei service configuration, upstream
release keys, official-app passkeys, Google Play billing, or Telegram's APK update
channel. Ordinary Telegram connectivity is separate from the local settings
import, whose contract is documented in `../docs/android/offline-import.md`.

### Android fork release numbering

`overlay/televim-version.properties` is the committed fork version source:
`TELEVIM_VERSION_CODE` is the base code and `TELEVIM_VERSION_NAME` is the display
name. Increment the base for every successive signed release and refresh this
file's SHA-256 in `source.json`. Keep the file as the two ASCII lines shown,
with LF endings and a final newline. Upstream `APP_VERSION_*` and Desktop's
`Telegram/build/version` remain independent; changing a tag or display name
alone does not make an APK eligible for an update.

The base must be an integer from `1` through `209999999`. The existing `afat`
flavor computes `base * 10 + 9`, at most `2099999999`, below Android's
`2100000000` limit and signed 32-bit maximum. All supported ABI selections use
suffix `9`, including arm64-only and universal: they have equal codes at the
same base. Increase the base before offering one as an update to the other,
and keep the device's ABI in the replacement APK. Do not renumber flavor
suffixes to manufacture an upgrade. Incrementing the base orders a new release
above every decimal ABI suffix of the preceding base.

The first fork base `7241` produces `72419`, above the preceding `72409` from
base `7240`. Before publication compare with the highest code already shipped
under the persistent `org.informativus.televim` package and signing certificate;
a privately overridden higher code needs a still higher release base. Preserve
that package and certificate so Android can update in place. A debug-signed
installation is not interchangeable with the release-signed application.

Both `android/build.sh release` and the signed workflow consume the committed
file through the prepared overlay. Missing or invalid fork versions fail;
there is no upstream fallback. Explicit Gradle `-PtelevimVersionCode` and
`-PtelevimVersionName` overrides retain priority, including Gradle's standard
`ORG_GRADLE_PROJECT_televimVersionCode` and
`ORG_GRADLE_PROJECT_televimVersionName` environment mechanism. Empty or invalid
overrides fail. These overrides are for explicit local builds; their actual
versions remain in Gradle's APK `output-metadata.json`. Official signed workflow
artifacts must match the committed code and name and cannot publish an override.

Preparation records `release_version` with the committed base, display name,
expected `afat` APK code and version-file SHA-256. The signed workflow checks
actual APK badging against this record and emits it beside the actual APK
version, source identity and artifact hash. The catalog publisher reads the same
fork source, requires matching artifact metadata, and retains its monotonic
version and identical-source checks. A local preparation record describes the
committed input; it is not a signed APK verification receipt.

Android numbering does not establish feature parity. Final cross-client delivery
still needs the matching reviewed Desktop and Android artifacts and the runtime
acceptance described in `../docs/android/feature-parity.md` and
`../docs/automatic-updates.md`.

### Localization

We moved all translations to https://translations.telegram.org/en/android/. Please use it.
