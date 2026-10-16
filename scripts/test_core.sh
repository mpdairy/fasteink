#!/usr/bin/env bash
# Builds and runs the C core's desktop checks against a fake driver.
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g -Icore/include \
  core/src/fasteink.c core/src/rotate.c core/tests/core_test.c -o "$out/core_test"
"$out/core_test"
