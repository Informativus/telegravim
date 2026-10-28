#!/bin/bash
set -euo pipefail

android_root="$(cd "$(dirname "$0")/.." && pwd)"
primary_root="${TELEVIM_PRIMARY_ROOT:?Use the monorepo android/build.sh entry point}"
variant="${1:-debug}"
abis="${2:-arm64-v8a}"
case "$variant" in
    debug) task=assembleAfatDebug ;;
    release) task=assembleAfatStandalone ;;
    *) echo 'Usage: scripts/build.sh debug|release [arm64-v8a|comma-separated ABIs]' >&2; exit 1 ;;
esac

export GRADLE_USER_HOME="${TELEVIM_GRADLE_HOME:-${GRADLE_USER_HOME:-$primary_root/.local/build-cache/gradle-android}}"
export TELEVIM_BUILD_ROOT="${TELEVIM_BUILD_ROOT:-$primary_root/.local/android-build}"
export CMAKE_BUILD_PARALLEL_LEVEL=2
if [ -z "${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}" ] && [ -d "$primary_root/.local/private/android-toolchain/sdk" ]; then
    export ANDROID_SDK_ROOT="$primary_root/.local/private/android-toolchain/sdk"
else
    export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
fi
if [ -z "${JAVA_HOME:-}" ] && [ -f "$primary_root/.local/private/android-toolchain/java-home.txt" ]; then
    JAVA_HOME="$(cat "$primary_root/.local/private/android-toolchain/java-home.txt")"
    export JAVA_HOME
fi
if [ -z "${TELEVIM_CONFIG_FILE:-}" ] && [ -f "$primary_root/.local/private/android-release/api.env" ]; then
    export TELEVIM_CONFIG_FILE="$primary_root/.local/private/android-release/api.env"
fi
if [ "$variant" = debug ] && [ ! -f "$TELEVIM_BUILD_ROOT/debug.keystore" ]; then
    mkdir -p "$TELEVIM_BUILD_ROOT"
    keytool_command=keytool
    if [ -n "${JAVA_HOME:-}" ]; then
        keytool_command="$JAVA_HOME/bin/keytool"
    fi
    "$keytool_command" -genkeypair -keystore "$TELEVIM_BUILD_ROOT/debug.keystore" \
        -storepass android -keypass android -alias androiddebugkey -keyalg RSA \
        -keysize 3072 -validity 10000 -dname 'CN=Televim Development,O=Televim,C=US' \
        >/dev/null 2>&1
    chmod 600 "$TELEVIM_BUILD_ROOT/debug.keystore"
fi
cd "$android_root"
exec ./gradlew --no-daemon --no-parallel --max-workers=2 \
    --project-cache-dir "$TELEVIM_BUILD_ROOT/gradle-cache" \
    "-PtelevimAbis=$abis" ":TMessagesProj_AppStandalone:$task"
