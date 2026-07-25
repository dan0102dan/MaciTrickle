/* `port` rule values: a protocol plus a destination port or port range.
 *
 * This is the one rule type that cannot go through an ipset. Every other
 * type ends up as a set of addresses, because that is all the packet path
 * has to work with; a port lives in the transport header, so it is matched
 * in the group's iptables chain instead (`-p tcp --dport 22`). That makes
 * it useful only where the chain has somewhere to put such a condition --
 * inside an except-group, as one more reason to leave a packet alone. In a
 * normal group there is nothing to attach it to and it is inert.
 *
 * Accepted forms (case-insensitive protocol):
 *   tcp/22          single port
 *   udp/53          single port
 *   tcp/1000-2000   inclusive range
 * Port 0 is rejected, as is a range whose end precedes its start.
 */
#ifndef MAGITRICKLE_PORTRULE_H
#define MAGITRICKLE_PORTRULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mt_port_rule {
    char proto[4]; /* "tcp" or "udp", NUL-terminated */
    uint16_t lo;
    uint16_t hi; /* == lo for a single port */
} mt_port_rule_t;

/* False (leaving *out untouched) when `value` is not a well-formed port
 * rule. */
bool mt_port_rule_parse(const char *value, mt_port_rule_t *out);

/* Renders the `--dport` argument for this rule: "22" or "1000:2000"
 * (iptables' range separator, not the dash used in config). buf must hold
 * at least 12 bytes. Returns buf. */
const char *mt_port_rule_dport(const mt_port_rule_t *r, char *buf, size_t buf_len);

#endif /* MAGITRICKLE_PORTRULE_H */
