/* DNS MITM proxy transport (compatibility-contract §4).
 *
 * Concurrency model: runs entirely on the caller's mt_loop (epoll), no
 * per-request threads. UDP is a single non-blocking socket read in a loop;
 * each datagram is forwarded to upstream over a pooled non-blocking socket
 * whose readiness re-enters the loop; the reply is written back with the
 * original destination as source address (IP_PKTINFO / IPV6_PKTINFO), same
 * as the Go backend. TCP accepts connections, reads exactly one length-
 * prefixed query per connection, forwards, writes the length-prefixed
 * answer, and closes (one query per connection — the contract).
 *
 * Backpressure: a bounded in-flight budget (max_concurrent) caps
 * outstanding upstream exchanges; when full, new datagrams are dropped
 * (UDP clients retry) and TCP accepts are paused — mirroring the Go
 * semaphore's effect (no unbounded queue, client silence on overload).
 *
 * Hooks mirror dns.go: request hook may answer a single-PTR query locally
 * (fake PTR), response hook strips AAAA and invokes the message callback
 * for cache/ipset population. The callback receives the parsed upstream
 * response; wiring it to cache/rules is Phase 4.
 */
#ifndef MAGITRICKLE_DNSPROXY_H
#define MAGITRICKLE_DNSPROXY_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/dnswire.h"
#include "magitrickle/err.h"
#include "magitrickle/loop.h"

typedef struct mt_dnsproxy mt_dnsproxy_t;

typedef struct mt_dnsproxy_config {
    const char *listen_addr;   /* e.g. "[::]" or "0.0.0.0" */
    uint16_t listen_port;      /* 3553 */
    const char *upstream_addr; /* "127.0.0.1" */
    uint16_t upstream_port;    /* 53 */
    uint32_t timeout_ms;       /* per-request upstream deadline */
    uint32_t max_concurrent;   /* in-flight budget (semaphore equivalent) */
    uint32_t max_idle_conns;   /* idle upstream sockets kept per transport */
    bool disable_fake_ptr;
    bool disable_drop_aaaa;
} mt_dnsproxy_config_t;

/* Called for every successfully parsed upstream response (network is
 * "udp"/"tcp", client addr string for logging). The proxy owns msg; the
 * callback must not free or retain it. Phase 4 wires cache + matching. */
typedef void (*mt_dnsproxy_msg_cb)(const mt_dns_msg_t *msg,
                                   const char *client_addr,
                                   const char *network, void *ud);

mt_err_t mt_dnsproxy_create(const mt_dnsproxy_config_t *cfg, mt_loop_t *loop,
                            mt_dnsproxy_msg_cb cb, void *cb_ud,
                            mt_dnsproxy_t **out);
void mt_dnsproxy_destroy(mt_dnsproxy_t *p);

/* Bind + register listeners on the loop. */
mt_err_t mt_dnsproxy_start(mt_dnsproxy_t *p);

/* Observability. */
uint64_t mt_dnsproxy_dropped(const mt_dnsproxy_t *p);
uint64_t mt_dnsproxy_inflight(const mt_dnsproxy_t *p);

#endif /* MAGITRICKLE_DNSPROXY_H */
