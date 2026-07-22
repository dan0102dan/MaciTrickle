/* Unified error model for the MagiTrickle C backend.
 *
 * Functions return mt_err_t (MT_OK == 0). Errors never abort the process;
 * a failure affecting a single request/connection must stay contained to it.
 * When a syscall fails, capture errno at the failure site (mt_err_from_errno
 * or log it) — do not let intermediate calls clobber it.
 */
#ifndef MAGITRICKLE_ERR_H
#define MAGITRICKLE_ERR_H

typedef enum mt_err {
    MT_OK = 0,
    MT_ERR_NOMEM,   /* allocation failure */
    MT_ERR_INVAL,   /* invalid argument / malformed input */
    MT_ERR_IO,      /* I/O or syscall failure (see errno at call site) */
    MT_ERR_AGAIN,   /* transient: retry later (would block) */
    MT_ERR_LIMIT,   /* bounded resource exhausted (queue full, cap hit) */
    MT_ERR_TIMEOUT, /* deadline expired */
    MT_ERR_CLOSED,  /* object already shut down */
    MT_ERR_EXIST,   /* duplicate / already present */
    MT_ERR_NOENT,   /* not found */
    MT_ERR_PROTO,   /* protocol violation (DNS/HTTP/netlink parse) */
    MT_ERR_STATE,   /* operation invalid in current lifecycle state */
    MT_ERR_SYS,     /* unclassified system error */
} mt_err_t;

/* Static string for an error code (never NULL). */
const char *mt_err_str(mt_err_t err);

/* Map an errno value to the closest mt_err_t. */
mt_err_t mt_err_from_errno(int errnum);

#endif /* MAGITRICKLE_ERR_H */
