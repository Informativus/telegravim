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
The Android workflow currently produces a verified signed build artifact.
Publishing that APK into the matching Desktop release is a separate release step.
A workflow artifact alone is not a published GitHub Release.

The fork does not use upstream Google/Huawei service configuration, upstream
release keys, official-app passkeys, Google Play billing, or Telegram's APK update
channel. Ordinary Telegram connectivity is separate from the local settings
import, whose contract is documented in `../docs/android/offline-import.md`.

### Localization

We moved all translations to https://translations.telegram.org/en/android/. Please use it.
