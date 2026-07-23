/* GET/PUT/POST /api/v1/subscriptions and DELETE
 * /api/v1/subscriptions/{subscriptionID} — port of the pure config-
 * mutation slice of api/v1/subscription_handlers.go and
 * subscription_converters.go (compatibility-contract.md §2's
 * subscriptions row).
 *
 * Deliberately NOT implemented here (need libcurl, Phase 7 scope, see
 * decisions.md): POST /api/v1/subscriptions/{id}/sync (fetch+parse the
 * remote list) and GET /api/v1/subscriptions/rules?url= (fetch+parse
 * without persisting). Neither route is registered, so a request to
 * either 404s via the normal not-found path rather than being faked.
 */
#ifndef MAGITRICKLE_SUBSCRIPTIONS_API_H
#define MAGITRICKLE_SUBSCRIPTIONS_API_H

#include "magitrickle/app.h"
#include "magitrickle/httpd.h"

typedef struct mt_subs_ctx {
    mt_app_t *app;
    /* Same meaning as mt_groups_ctx_t's fields (groups.h). Note the
     * subscriptions handlers' ?save= default is the OPPOSITE of the
     * groups handlers': it saves unless save=false is explicit (matches
     * Go's `r.URL.Query().Get("save") != "false"`, vs. groups' `== "true"`
     * -- a real asymmetry already present in the Go handlers, not
     * introduced here). */
    const char *config_path;
    const char *config_version;
} mt_subs_ctx_t;

/* Registers GET/PUT/POST /api/v1/subscriptions and DELETE
 * /api/v1/subscriptions/{subscriptionID} on `h`. `ctx` must outlive the
 * server. */
void mt_subs_register_routes(mt_httpd_t *h, mt_subs_ctx_t *ctx);

#endif /* MAGITRICKLE_SUBSCRIPTIONS_API_H */
