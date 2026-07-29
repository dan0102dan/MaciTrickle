/* See cancel.h. */
#include "magitrickle/cancel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct mt_cancel {
    atomic_bool raised;
    int rd;
    int wr;
};

static int set_nonblock_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { return -1; }
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) { return -1; }
    return 0;
}

mt_cancel_t *mt_cancel_new(void) {
    mt_cancel_t *c = calloc(1, sizeof(*c));
    if (!c) { return NULL; }

    int fds[2];
    if (pipe(fds) != 0) {
        free(c);
        return NULL;
    }
    if (set_nonblock_cloexec(fds[0]) != 0 || set_nonblock_cloexec(fds[1]) != 0) {
        close(fds[0]);
        close(fds[1]);
        free(c);
        return NULL;
    }

    c->rd = fds[0];
    c->wr = fds[1];
    atomic_init(&c->raised, false);
    return c;
}

void mt_cancel_free(mt_cancel_t *c) {
    if (!c) { return; }
    close(c->rd);
    close(c->wr);
    free(c);
}

void mt_cancel_raise(mt_cancel_t *c) {
    if (!c) { return; }

    /* Only the false->true transition writes, so the pipe holds at most
     * one byte no matter how many restart requests pile up. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&c->raised, &expected, true)) { return; }

    const uint8_t byte = 1;
    ssize_t n;
    do {
        n = write(c->wr, &byte, 1);
    } while (n < 0 && errno == EINTR);
}

void mt_cancel_clear(mt_cancel_t *c) {
    if (!c) { return; }

    atomic_store(&c->raised, false);

    uint8_t buf[8];
    for (;;) {
        ssize_t n = read(c->rd, buf, sizeof(buf));
        if (n > 0) { continue; }
        if (n < 0 && errno == EINTR) { continue; }
        break;
    }
}

bool mt_cancel_raised(const mt_cancel_t *c) {
    if (!c) { return false; }
    return atomic_load(&((mt_cancel_t *)c)->raised);
}

int mt_cancel_fd(const mt_cancel_t *c) {
    return c ? c->rd : -1;
}
