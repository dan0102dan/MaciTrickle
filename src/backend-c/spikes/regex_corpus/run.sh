#!/bin/sh
# Regex compatibility spike: dlclark/regexp2 (Go oracle) vs PCRE2.
# Prints a unified diff of per-case results; exit 0 when identical.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/out}"
mkdir -p "$OUT"

( cd "$DIR/oracle_go" && go mod tidy >/dev/null 2>&1 && go build -o "$OUT/oracle" . )
cc -O2 -Wall -Wextra -o "$OUT/pcre2_runner" "$DIR/pcre2_runner.c" $(pcre2-config --libs8 --cflags)

"$OUT/oracle" < "$DIR/corpus.tsv" > "$OUT/regexp2.tsv"
"$OUT/pcre2_runner" < "$DIR/corpus.tsv" > "$OUT/pcre2.tsv"

if diff -u "$OUT/regexp2.tsv" "$OUT/pcre2.tsv" > "$OUT/divergence.diff"; then
    echo "IDENTICAL: $(grep -c . "$OUT/regexp2.tsv") cases, no divergence"
else
    echo "DIVERGENCES FOUND:"
    cat "$OUT/divergence.diff"
    exit 1
fi
