/* See portrule.h. */
#include "magitrickle/portrule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parses a decimal 1..65535, rejecting empty input, leading signs and
 * anything that is not a digit. *end is left on the first byte not
 * consumed. */
static bool parse_port(const char *s, const char **end, uint16_t *out) {
    if (*s < '0' || *s > '9') { return false; }

    unsigned long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 65535) { return false; }
        p++;
    }
    if (v == 0) { return false; }

    *out = (uint16_t)v;
    *end = p;
    return true;
}

static bool proto_eq(const char *s, size_t len, const char *want) {
    if (len != strlen(want)) { return false; }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
        if (c != want[i]) { return false; }
    }
    return true;
}

bool mt_port_rule_parse(const char *value, mt_port_rule_t *out) {
    if (!value || !out) { return false; }

    const char *slash = strchr(value, '/');
    if (!slash || slash == value) { return false; }

    size_t proto_len = (size_t)(slash - value);
    const char *proto;
    if (proto_eq(value, proto_len, "tcp")) {
        proto = "tcp";
    } else if (proto_eq(value, proto_len, "udp")) {
        proto = "udp";
    } else {
        return false;
    }

    const char *p = slash + 1;
    uint16_t lo = 0, hi = 0;
    if (!parse_port(p, &p, &lo)) { return false; }

    if (*p == '-') {
        p++;
        if (!parse_port(p, &p, &hi)) { return false; }
        if (hi < lo) { return false; }
    } else {
        hi = lo;
    }
    if (*p != '\0') { return false; }

    memcpy(out->proto, proto, 4);
    out->lo = lo;
    out->hi = hi;
    return true;
}

const char *mt_port_rule_dport(const mt_port_rule_t *r, char *buf, size_t buf_len) {
    if (r->lo == r->hi) {
        snprintf(buf, buf_len, "%u", (unsigned)r->lo);
    } else {
        snprintf(buf, buf_len, "%u:%u", (unsigned)r->lo, (unsigned)r->hi);
    }
    return buf;
}
