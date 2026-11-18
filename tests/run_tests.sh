#!/bin/bash
# Run all Slicer tests. Called by "make test" (which builds the binaries).
#
#   1. Model unit tests (C)
#   2. C slicing / SFZ output compared with the Python reference
#   3. Plugin driven by a jalv-like test host (C)
#   4. Plugin description checked against how Zynthian reads it (Python)
#   5. Widget logic with fake Zynthian modules (Python)
#
# Needs Python 3 with numpy, soundfile and rdflib (all present on Zynthian).
# Set PYTHON to choose the interpreter.

set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
PYTHON="${PYTHON:-python3}"
WORK="$DIR/build/test-work"

cd "$DIR" || exit 1
rm -rf "$WORK"
mkdir -p "$WORK"

failed=0
run() {
    local name="$1"
    shift
    echo
    echo "=== $name ==="
    if ! "$@"; then
        echo "FAILED: $name" >&2
        failed=1
    fi
}

run "Model unit tests" build/test_model unit
run "Comparison with the Python slicer" "$PYTHON" tests/compare.py build/test_model "$WORK/audio"
run "Plugin host tests" build/test_host slicer.lv2/slicer.so "$WORK/audio/loop44.wav" "$WORK/host"
run "Plugin description (TTL) tests" "$PYTHON" tests/test_ttl.py
run "Widget tests" "$PYTHON" tests/test_widget.py

echo
if [ "$failed" -ne 0 ]; then
    echo "Some tests failed" >&2
    exit 1
fi
echo "All tests passed"
