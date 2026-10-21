#!/bin/sh
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$TEST_DIR/../.." && pwd)
TEST_BINARY=$(mktemp "${TMPDIR:-/tmp}/sekonic-core-tests.XXXXXX")
trap 'rm -f "$TEST_BINARY"' EXIT HUP INT TERM
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$PROJECT_DIR/Sources/DeviceCore" \
  "$PROJECT_DIR/Sources/DeviceCore/LanguageCore.c" "$TEST_DIR/CoreTests.c" \
  -o "$TEST_BINARY"
"$TEST_BINARY"
