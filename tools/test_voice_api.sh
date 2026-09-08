#!/usr/bin/env bash
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"
mkdir -p build
gcc -O2 -Wall -Wextra -std=c11 \
    -o build/test_voice_api \
    tools/test_voice_api.c source/voice_api.c
./build/test_voice_api
