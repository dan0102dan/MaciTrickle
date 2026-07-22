/* magitrickled-c — Phase 4 daemon.
 *
 * Loads the YAML config (defaults + overlay), builds a rule-set snapshot
 * from its user groups, starts the epoll DNS MITM proxy, and processes
 * every upstream response through the records cache + rule matching
 * pipeline (dns.go's handleMessage, ported — see dnspipeline.h). This is
 * NOT yet the full production daemon: the match sink below only logs
 * (netfilter/ipset lands in Phase 5); HTTP/Unix API, subscriptions, and
 * the netlink watcher land in later phases. The Go backend remains
 * authoritative until the migration completes (docs/c-rewrite/).
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "magitrickle/dns_cache.h"
#include "magitrickle/dnspipeline.h"
#include "magitrickle/dnsproxy.h"
#include "magitrickle/log.h"
#include "magitrickle/loop.h"
#include "magitrickle/rulesnap.h"
#include "magitrickle/version.h"
#include "magitrickle/yamlio.h"

#ifndef MT_CONFIG_PATH
#define MT_CONFIG_PATH "/var/lib/magitrickle/config.yaml"
#endif

#define MT_CACHE_CLEANUP_INTERVAL_MS 30000

struct daemon {
    mt_loop_t *loop;
    mt_dnsproxy_t *proxy;
    mt_cache_t *cache;
    mt_dns_pipeline_t *pipeline;
};

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

static void on_signal(mt_loop_t *loop, int signo, void *ud)
{
    (void)ud;
    if (signo == SIGHUP) {
        MT_INFO("received signal: hangup (reload not implemented yet)");
        return;
    }
    MT_INFO("received signal: %s", strsignal(signo));
    mt_loop_stop(loop);
}

static void on_cache_cleanup_timer(mt_loop_t *loop, void *ud)
{
    (void)loop;
    struct daemon *d = ud;
    mt_cache_cleanup(d->cache, now_unix());
}

/* Phase 4 sink: log what Phase 5 will turn into an ipset add. Mirrors the
 * observable content of Go's "added to routing" / "added alias" log
 * lines (log *content* is not a compatibility-contract item — only level
 * names are, per compatibility-contract.md §11). */
static void on_match(const mt_match_action_t *action, void *ud)
{
    (void)ud;
    char addr_str[64];
    if (action->addr_len == 4) {
        snprintf(addr_str, sizeof(addr_str), "%u.%u.%u.%u", action->addr[0],
                 action->addr[1], action->addr[2], action->addr[3]);
    } else {
        snprintf(addr_str, sizeof(addr_str),
                 "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                 "%02x%02x:%02x%02x",
                 action->addr[0], action->addr[1], action->addr[2],
                 action->addr[3], action->addr[4], action->addr[5],
                 action->addr[6], action->addr[7], action->addr[8],
                 action->addr[9], action->addr[10], action->addr[11],
                 action->addr[12], action->addr[13], action->addr[14],
                 action->addr[15]);
    }
    MT_INFO(
        "would add to routing: kind=%s domain=%s matched=%s group=%s "
        "addr=%s ttl=%u",
        action->record_kind, action->record_domain, action->matched_name,
        action->group_name, addr_str, action->ttl_seconds);
}

static void on_response(const mt_dns_msg_t *msg, const char *client_addr,
                        const char *network, void *ud)
{
    (void)client_addr;
    struct daemon *d = ud;
    MT_DEBUG("upstream response: id=%u answers=%zu network=%s", msg->id,
             msg->n_answers, network);
    mt_dns_pipeline_handle_message(d->pipeline, msg, now_unix());
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", MT_VERSION);
        return 0;
    }
    const char *config_path = MT_CONFIG_PATH;
    if (argc > 2 && strcmp(argv[1], "--config") == 0) {
        config_path = argv[2];
    }

    MT_INFO("starting MagiTrickle daemon (C) version=%s", MT_VERSION);

    mt_config_t cfg;
    if (mt_config_init_defaults(&cfg) != MT_OK) {
        MT_ERROR("failed to init config defaults");
        return 1;
    }
    mt_err_t err = mt_config_load_file(&cfg, config_path);
    if (err == MT_ERR_NOENT) {
        MT_INFO("config file %s not found, using defaults", config_path);
    } else if (err != MT_OK) {
        MT_ERROR("failed to load config %s: %s", config_path,
                 mt_err_str(err));
        mt_config_clear(&cfg);
        return 1;
    }
    mt_log_set_level(mt_log_level_from_str(cfg.app.log_level));

    struct daemon d = {0};

    if (mt_loop_create(&d.loop) != MT_OK) {
        MT_ERROR("failed to create event loop");
        mt_config_clear(&cfg);
        return 1;
    }

    d.cache = mt_cache_create(0);
    if (d.cache == NULL) {
        MT_ERROR("failed to create records cache");
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }

    uint32_t additional_ttl = (uint32_t)(cfg.app.netfilter.ipset.additional_ttl /
                                         MT_DURATION_SEC);
    d.pipeline = mt_dns_pipeline_create(d.cache, additional_ttl, on_match,
                                        NULL);
    if (d.pipeline == NULL) {
        MT_ERROR("failed to create DNS pipeline");
        mt_cache_destroy(d.cache);
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }
    mt_dns_pipeline_set_snapshot(d.pipeline, mt_ruleset_snapshot_build(&cfg));

    int cleanup_timer = 0;
    if (mt_loop_add_timer(d.loop, MT_CACHE_CLEANUP_INTERVAL_MS,
                          MT_CACHE_CLEANUP_INTERVAL_MS,
                          on_cache_cleanup_timer, &d,
                          &cleanup_timer) != MT_OK) {
        MT_ERROR("failed to schedule cache cleanup");
        mt_dns_pipeline_destroy(d.pipeline);
        mt_cache_destroy(d.cache);
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }

    mt_dnsproxy_config_t pcfg = {
        .listen_addr = cfg.app.dns_proxy.host.address,
        .listen_port = cfg.app.dns_proxy.host.port,
        .upstream_addr = cfg.app.dns_proxy.upstream.address,
        .upstream_port = cfg.app.dns_proxy.upstream.port,
        .timeout_ms = (uint32_t)(cfg.app.dns_proxy.timeout / MT_DURATION_MS),
        .max_concurrent = (uint32_t)cfg.app.dns_proxy.max_concurrent,
        .max_idle_conns = (uint32_t)cfg.app.dns_proxy.max_idle_conns,
        .disable_fake_ptr = cfg.app.dns_proxy.disable_fake_ptr,
        .disable_drop_aaaa = cfg.app.dns_proxy.disable_drop_aaaa,
    };
    if (pcfg.max_concurrent == 0) {
        pcfg.max_concurrent = 100;
    }
    if (pcfg.timeout_ms == 0) {
        pcfg.timeout_ms = 5000;
    }

    if (mt_dnsproxy_create(&pcfg, d.loop, on_response, &d, &d.proxy) !=
        MT_OK) {
        MT_ERROR("failed to create DNS proxy");
        mt_dns_pipeline_destroy(d.pipeline);
        mt_cache_destroy(d.cache);
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }
    err = mt_dnsproxy_start(d.proxy);
    if (err != MT_OK) {
        MT_ERROR("failed to start DNS proxy: %s", mt_err_str(err));
        mt_dnsproxy_destroy(d.proxy);
        mt_dns_pipeline_destroy(d.pipeline);
        mt_cache_destroy(d.cache);
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (mt_loop_add_signals(d.loop, &set, on_signal, NULL) != MT_OK) {
        MT_ERROR("failed to subscribe to signals");
        mt_dnsproxy_destroy(d.proxy);
        mt_dns_pipeline_destroy(d.pipeline);
        mt_cache_destroy(d.cache);
        mt_loop_destroy(d.loop);
        mt_config_clear(&cfg);
        return 1;
    }

    MT_INFO("service started");
    err = mt_loop_run(d.loop);
    if (err != MT_OK) {
        MT_ERROR("event loop failed: %s", mt_err_str(err));
    }

    MT_INFO("service stopped");
    mt_dnsproxy_destroy(d.proxy);
    mt_dns_pipeline_destroy(d.pipeline);
    mt_cache_destroy(d.cache);
    mt_loop_destroy(d.loop);
    mt_config_clear(&cfg);
    return err == MT_OK ? 0 : 1;
}
