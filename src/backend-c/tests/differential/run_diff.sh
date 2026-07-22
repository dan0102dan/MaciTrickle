#!/bin/sh
# Differential test runner: Go reference backend vs C implementation.
#
# Suites:
#   regex    — regexp2 vs PCRE2 corpus (Phase 1 spike; known divergences
#              frozen in spikes/regex_corpus/known_divergences.tsv)
#   yaml     — libyaml emit shape vs go-yaml v2 (Phase 1 spike)
#   config   — load+save cross-comparison over fixtures/ (Phase 2):
#              for every fixture, Go (real App.LoadConfig+SaveConfig) and
#              C (mt-configtool resave) must produce byte-identical output
#              (or both fail), plus C-save must reload cleanly in Go
#   match    — rule matching corpus (models.Rule.IsMatch vs mt_rule_matcher)
#   subparse — subscription list parsing corpus
#
# Requires root for the config suite (the Go oracle exercises the real
# /var/lib/magitrickle path). Exit non-zero on any unexpected divergence.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"
OUT="$DIR/out"
mkdir -p "$OUT"

fail=0

echo "== differential: regex (regexp2 vs PCRE2)"
if ! sh "$BACKEND_C_DIR/spikes/regex_corpus/run.sh"; then
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

echo "== differential: building tools"
( cd "$BACKEND_C_DIR" && make >/dev/null )
CONFIGTOOL="$BACKEND_C_DIR/build/host/mt-configtool"
( cd "$DIR/oracle_go" && go mod tidy >/dev/null 2>&1 && \
  go build -ldflags "-X 'magitrickle/constant.Version=0.99.0'" \
      -o "$OUT/oracle" . )
ORACLE="$OUT/oracle"

echo "== differential: config load/save fixtures"
for fixture in "$DIR"/fixtures/*.yaml; do
    name=$(basename "$fixture" .yaml)
    "$ORACLE" resave "$fixture" > "$OUT/$name.go.yaml"
    "$CONFIGTOOL" resave "$fixture" 0.99.0 > "$OUT/$name.c.yaml"
    if ! diff -u "$OUT/$name.go.yaml" "$OUT/$name.c.yaml" \
         > "$OUT/$name.diff" 2>&1; then
        echo "   DIVERGENCE in $name:"
        head -20 "$OUT/$name.diff"
        fail=1
    else
        echo "   $name: OK"
    fi
    # cross: C output must reload identically through Go (skip failures)
    if ! grep -q '^ERROR$' "$OUT/$name.c.yaml"; then
        "$ORACLE" resave "$OUT/$name.c.yaml" > "$OUT/$name.cross.yaml"
        if ! diff -u "$OUT/$name.go.yaml" "$OUT/$name.cross.yaml" \
             > "$OUT/$name.cross.diff" 2>&1; then
            echo "   CROSS-RELOAD divergence in $name"
            head -10 "$OUT/$name.cross.diff"
            fail=1
        fi
    fi
done

echo "== differential: missing-file behaviour (defaults)"
"$ORACLE" resave -missing- > "$OUT/missing.go.yaml"
"$CONFIGTOOL" resave /nonexistent/config.yaml 0.99.0 > "$OUT/missing.c.yaml"
if ! diff -u "$OUT/missing.go.yaml" "$OUT/missing.c.yaml" \
     > "$OUT/missing.diff" 2>&1; then
    echo "   DIVERGENCE in defaults:"
    head -20 "$OUT/missing.diff"
    fail=1
else
    echo "   defaults: OK"
fi

echo "== differential: rule matching corpus"
"$ORACLE" match < "$DIR/corpus/match_corpus.tsv" > "$OUT/match.go.tsv"
"$CONFIGTOOL" match < "$DIR/corpus/match_corpus.tsv" > "$OUT/match.c.tsv"
if ! diff -u "$OUT/match.go.tsv" "$OUT/match.c.tsv" \
     > "$OUT/match.diff" 2>&1; then
    echo "   DIVERGENCE:"
    cat "$OUT/match.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/match.go.tsv") cases: OK"
fi

echo "== differential: subscription parse corpus"
"$ORACLE" subparse < "$DIR/corpus/subparse_corpus.txt" > "$OUT/subparse.go.txt"
"$CONFIGTOOL" subparse < "$DIR/corpus/subparse_corpus.txt" > "$OUT/subparse.c.txt"
if ! diff -u "$OUT/subparse.go.txt" "$OUT/subparse.c.txt" \
     > "$OUT/subparse.diff" 2>&1; then
    echo "   DIVERGENCE:"
    cat "$OUT/subparse.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/subparse.go.txt") rules: OK"
fi

exit $fail
