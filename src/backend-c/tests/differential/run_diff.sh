#!/bin/sh
# Differential test runner (Phase 1 scaffold).
#
# Drives the Go reference backend and the C implementation with identical
# inputs and compares canonicalized outputs. Suites appear per phase:
#   Phase 1 (now): dependency parity spikes double as the first suites —
#     regex   : dlclark/regexp2 vs PCRE2 on the shared corpus
#     yaml    : go.yaml.in/yaml/v2 vs libyaml emit shape
#   Phase 2+: config load/save cross-products, rule matching corpus
#   Phase 3+: DNS wire corpus (semantic comparison)
#   Phase 6+: HTTP/Unix API contract replay
#
# Exit non-zero when any suite diverges. Divergence details land in each
# suite's out/divergence.diff.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"

fail=0

echo "== differential: regex (regexp2 vs PCRE2)"
if ! sh "$BACKEND_C_DIR/spikes/regex_corpus/run.sh"; then
    # Known, documented divergences are recorded in
    # spikes/regex_corpus/known_divergences.tsv; anything else is a failure.
    KNOWN="$BACKEND_C_DIR/spikes/regex_corpus/known_divergences.tsv"
    ACTUAL_NORM="$BACKEND_C_DIR/spikes/regex_corpus/out/divergence.norm"
    grep -E '^[+-]' "$BACKEND_C_DIR/spikes/regex_corpus/out/divergence.diff" \
        | grep -vE '^(\+\+\+|---)' > "$ACTUAL_NORM" || true
    if [ -f "$KNOWN" ] && \
       diff -u "$KNOWN" "$ACTUAL_NORM" >/dev/null 2>&1; then
        echo "   divergences match the documented known set — OK"
    else
        echo "   UNEXPECTED regex divergence (update decisions.md D-07 if intended)"
        fail=1
    fi
fi

echo "== differential: yaml emit (yaml.v2 vs libyaml)"
if ! sh "$BACKEND_C_DIR/spikes/yaml_emit/run.sh"; then
    fail=1
fi

exit $fail
