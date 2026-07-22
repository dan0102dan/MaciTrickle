#!/bin/sh
# libyaml emit spike: byte-compare libyaml output against Go yaml.v2 output
# for the same config data. Divergences drive the Phase 2 emitter shims.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/out}"
mkdir -p "$OUT"

( cd "$DIR/fixture_go" && go mod tidy >/dev/null 2>&1 && \
  go build -o "$OUT/fixture" . )
cc -O2 -Wall -Wextra -o "$OUT/emit_config" "$DIR/emit_config.c" -lyaml

"$OUT/fixture" > "$OUT/go-yaml-v2.yaml"
"$OUT/emit_config" > "$OUT/libyaml.yaml"

if diff -u "$OUT/go-yaml-v2.yaml" "$OUT/libyaml.yaml" > "$OUT/divergence.diff"; then
    echo "IDENTICAL ($(wc -c < "$OUT/go-yaml-v2.yaml") bytes)"
else
    echo "DIVERGENCES:"
    cat "$OUT/divergence.diff"
    exit 1
fi
