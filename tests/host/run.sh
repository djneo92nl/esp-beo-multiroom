#!/bin/sh
# Build the core on the host and compare it with beo's golden answers.
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-.}/esp32port_test"
c++ -std=c++17 -Wall -Wextra -o "$OUT" test_mozart.cpp ../../src/core/mozart_api.cpp ../../src/core/player_state.cpp
python3 compare_golden.py "$OUT" "$@"
