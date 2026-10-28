#!/bin/bash
set -euo pipefail

android_root="$(cd "$(dirname "$0")" && pwd)"
variant="${1:-debug}"
abis="${2:-arm64-v8a}"
if [ "$#" -gt 2 ]; then
    echo 'Usage: android/build.sh debug|release [comma-separated ABIs]' >&2
    exit 1
fi
exec "${TELEVIM_PYTHON:-python3}" "$android_root/prepare.py" --build "$variant" --abis "$abis"
