/* magitrickled-c — Phase 1 skeleton daemon.
 *
 * Validates the foundation only: logging, event loop, signal-driven
 * shutdown. It is NOT the production daemon yet; the Go backend remains
 * authoritative until the migration completes (docs/c-rewrite/).
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "magitrickle/lifecycle.h"
#include "magitrickle/log.h"
#include "magitrickle/loop.h"
#include "magitrickle/version.h"

static void on_signal(mt_loop_t *loop, int signo, void *ud)
{
    (void)ud;
    if (signo == SIGHUP) {
        /* Phase 2+: config reload. For now just acknowledge. */
        MT_INFO("received signal: hangup (reload not implemented yet)");
        return;
    }
    MT_INFO("received signal: %s", strsignal(signo));
    mt_loop_stop(loop);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", MT_VERSION);
        return 0;
    }

    MT_INFO("starting MagiTrickle daemon (C skeleton) version=%s",
            MT_VERSION);

    mt_loop_t *loop = NULL;
    mt_err_t err = mt_loop_create(&loop);
    if (err != MT_OK) {
        MT_ERROR("failed to create event loop: %s", mt_err_str(err));
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    err = mt_loop_add_signals(loop, &set, on_signal, NULL);
    if (err != MT_OK) {
        MT_ERROR("failed to subscribe to signals: %s", mt_err_str(err));
        mt_loop_destroy(loop);
        return 1;
    }

    MT_INFO("starting service");
    err = mt_loop_run(loop);
    if (err != MT_OK) {
        MT_ERROR("event loop failed: %s", mt_err_str(err));
    }

    MT_INFO("service stopped");
    mt_loop_destroy(loop);
    return err == MT_OK ? 0 : 1;
}
