/* magitrickled-c — Phase 3 daemon.
 *
 * Loads the YAML config (defaults + overlay), starts the epoll DNS MITM
 * proxy, and serves UDP+TCP until a signal arrives. This is NOT yet the
 * full production daemon — netfilter/ipset, the HTTP/Unix API, subscriptions
 * and the netlink watcher land in later phases; the Go backend remains
 * authoritative until the migration completes (docs/c-rewrite/).
 *
 * The response callback currently only logs at debug level; wiring it to
 * the records cache + rule matching + ipset is Phase 4/5.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "magitrickle/dnsproxy.h"
#include "magitrickle/log.h"
#include "magitrickle/loop.h"
#include "magitrickle/version.h"
#include "magitrickle/yamlio.h"

#ifndef MT_CONFIG_PATH
#define MT_CONFIG_PATH "/var/lib/magitrickle/config.yaml"
#endif

struct daemon {
    mt_loop_t *loop;
    mt_dnsproxy_t *proxy;
};

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

static void on_response(const mt_dns_msg_t *msg, const char *client_addr,
                        const char *network, void *ud)
{
    (void)client_addr;
    (void)ud;
    /* Phase 4 wires cache + rule matching + ipset here. */
    MT_DEBUG("upstream response: id=%u answers=%zu network=%s", msg->id,
             msg->n_answers, network);
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

    mt_loop_t *loop = NULL;
    if (mt_loop_create(&loop) != MT_OK) {
        MT_ERROR("failed to create event loop");
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

    mt_dnsproxy_t *proxy = NULL;
    if (mt_dnsproxy_create(&pcfg, loop, on_response, NULL, &proxy) != MT_OK) {
        MT_ERROR("failed to create DNS proxy");
        mt_loop_destroy(loop);
        mt_config_clear(&cfg);
        return 1;
    }
    err = mt_dnsproxy_start(proxy);
    if (err != MT_OK) {
        MT_ERROR("failed to start DNS proxy: %s", mt_err_str(err));
        mt_dnsproxy_destroy(proxy);
        mt_loop_destroy(loop);
        mt_config_clear(&cfg);
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (mt_loop_add_signals(loop, &set, on_signal, NULL) != MT_OK) {
        MT_ERROR("failed to subscribe to signals");
        mt_dnsproxy_destroy(proxy);
        mt_loop_destroy(loop);
        mt_config_clear(&cfg);
        return 1;
    }

    MT_INFO("service started");
    err = mt_loop_run(loop);
    if (err != MT_OK) {
        MT_ERROR("event loop failed: %s", mt_err_str(err));
    }

    MT_INFO("service stopped");
    mt_dnsproxy_destroy(proxy);
    mt_loop_destroy(loop);
    mt_config_clear(&cfg);
    return err == MT_OK ? 0 : 1;
}
