#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for test in buffer_core_test wave_decision_tests safe_policy_test observation_adapter_test; do
  g++ -std=c++20 -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti \
    -I"$root/src" "$root/tests/$test.cpp" -o "$tmp/$test"
  "$tmp/$test"
  echo "$test: PASS"
done
python3 - "$root" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
m = json.loads((root / 'mod.json').read_text())
assert m['gd']['ios'] == '2.2081' and m['geode'] == '5.11.2'
assert m['version'] == '0.3.0'
assert m['settings']['safe-mode']['default'] is True
assert set(m['settings']) == {'buffer-stage', 'enabled', 'safe-mode', 'show-debug-overlay'}
assert 'VERSION 0.3.0' in (root / 'CMakeLists.txt').read_text()
print('Source metadata: PASS (not an inspected .geode package)')
PY
