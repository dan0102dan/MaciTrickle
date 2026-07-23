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
#   dns      — DNS wire corpus (miekg/dns vs mt-dnstool: dump/stripaaaa/ptrcheck)
#   cache    — records cache script (recordsCache vs mt_cache; structural
#              comparison only, see cache_oracle_go for why)
#   http     — Phase 6 HTTP API contract: runs the real Go daemon and
#              magitrickled-c against byte-identical scratch configs,
#              drives each through the same fixed request sequence
#              (http_contract/contract.py), diffs the traces (see
#              run_http_diff.sh for what's normalized/redacted and why)
#
# Requires root for the config and http suites (the Go oracle/daemon
# exercise real /var/lib/magitrickle, real iptables). Exit non-zero on
# any unexpected divergence.
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
DNSTOOL="$BACKEND_C_DIR/build/host/mt-dnstool"
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

echo "== differential: DNS wire corpus (miekg/dns vs C)"
( cd "$DIR/dns_gen_go" && go mod tidy >/dev/null 2>&1 && go run . ) \
    > "$DIR/corpus/dns_corpus.hex"
( cd "$DIR/dns_oracle_go" && go mod tidy >/dev/null 2>&1 && \
  go build -o "$OUT/dns_oracle" . )
DNS_ORACLE="$OUT/dns_oracle"
DNS_CORPUS="$DIR/corpus/dns_corpus.hex"
for mode in dump stripaaaa ptrcheck; do
    "$DNS_ORACLE" "$mode" < "$DNS_CORPUS" > "$OUT/dns.$mode.go.txt"
    "$DNSTOOL" "$mode" < "$DNS_CORPUS" > "$OUT/dns.$mode.c.txt"
    if ! diff -u "$OUT/dns.$mode.go.txt" "$OUT/dns.$mode.c.txt" \
         > "$OUT/dns.$mode.diff" 2>&1; then
        echo "   DNS $mode DIVERGENCE:"
        head -30 "$OUT/dns.$mode.diff"
        fail=1
    else
        echo "   dns $mode: $(grep -c '^===' "$OUT/dns.$mode.go.txt") messages: OK"
    fi
done

echo "== differential: DNS records cache (recordsCache vs mt_cache)"
CACHETOOL="$BACKEND_C_DIR/build/host/mt-cachetool"
( cd "$DIR/cache_oracle_go" && go mod tidy >/dev/null 2>&1 && \
  go build -o "$OUT/cache_oracle" . )
CACHE_ORACLE="$OUT/cache_oracle"
CACHE_SCRIPT="$DIR/corpus/cache_script.txt"
"$CACHE_ORACLE" < "$CACHE_SCRIPT" > "$OUT/cache.go.txt"
"$CACHETOOL" < "$CACHE_SCRIPT" > "$OUT/cache.c.txt"
if ! diff -u "$OUT/cache.go.txt" "$OUT/cache.c.txt" > "$OUT/cache.diff" 2>&1; then
    echo "   DIVERGENCE:"
    cat "$OUT/cache.diff"
    fail=1
else
    echo "   $(grep -c . "$OUT/cache.go.txt") queries: OK"
fi

echo "== differential: HTTP API contract (Phase 6, Go daemon vs magitrickled-c)"
if ! sh "$DIR/run_http_diff.sh"; then
    fail=1
fi

exit $fail
