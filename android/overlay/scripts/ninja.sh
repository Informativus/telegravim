#!/bin/sh
set -eu

sdk_root=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
if [ -z "$sdk_root" ]; then
    echo 'Set ANDROID_SDK_ROOT to the configured Android SDK.' >&2
    exit 1
fi
exec "$sdk_root/cmake/3.22.1/bin/ninja" "$@" -j2
