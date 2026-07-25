/* Data models mirroring src/backend/models (Go).
 *
 * Ownership: every char* inside a model is owned by that model (strdup'd on
 * set, freed by the *_free function). Arrays own their elements. Models are
 * plain single-threaded data — concurrent access is the caller's problem
 * (the future core will publish immutable snapshots, decisions.md D-12).
 *
 * Contract notes (compatibility-contract.md §1):
 * - absent `enable` in YAML unmarshals to false (plain Go bool);
 * - rule types: domain | namespace | wildcard | regex | subnet | subnet6;
 * - group color is normalized at load: invalid -> "#ffffff", valid ->
 *   lowercased.
 */
#ifndef MAGITRICKLE_MODELS_H
#define MAGITRICKLE_MODELS_H

#include <stdbool.h>
#include <stdint.h>

#include "magitrickle/duration.h"
#include "magitrickle/err.h"
#include "magitrickle/id.h"

#define MT_RULE_DOMAIN    "domain"
#define MT_RULE_NAMESPACE "namespace"
#define MT_RULE_WILDCARD  "wildcard"
#define MT_RULE_REGEX     "regex"
#define MT_RULE_SUBNET    "subnet"
#define MT_RULE_SUBNET6   "subnet6"
/* Exception-only rule type: a port/protocol condition cannot be expressed
 * as ipset membership (the packet path only ever sees addresses), so it is
 * matched in the group's iptables chain instead. Meaningful only inside a
 * MT_GROUP_MODE_EXCEPT group; in a normal group it is inert, exactly like
 * any unrecognized type (see match.h's RK_NEVER). */
#define MT_RULE_PORT      "port"

/* Group routing modes (yaml/json key `mode`; absent == normal).
 *
 * normal: the group's rules select what goes through its interface --
 *         resolved addresses land in the group's ipset and the chain marks
 *         packets whose destination is in that set.
 * except: the group's rules select what does NOT go through it. The same
 *         ipset now holds the exceptions, the chain returns for them and
 *         marks everything else. Reads as "route everything through wg0
 *         except example.com, the LAN, and SSH" rather than as a negated
 *         matcher -- see decisions.md D-57. */
#define MT_GROUP_MODE_NORMAL "normal"
#define MT_GROUP_MODE_EXCEPT "except"

/* What happens to traffic an except-group excluded (key `onException`;
 * absent == continue).
 *
 * continue:  the packet is left unmarked by this group and other groups
 *            still get their say -- a later, more specific group can pick
 *            it up.
 * mainroute: the packet leaves on the system's main route, and no other
 *            group may claim it.  */
#define MT_GROUP_ONEXC_CONTINUE  "continue"
#define MT_GROUP_ONEXC_MAINROUTE "mainroute"

typedef struct mt_rule {
    mt_id_t id;
    char *name;
    char *type;
    char *rule;
    bool enable;
} mt_rule_t;

typedef struct mt_group {
    mt_id_t id;
    char *name;
    char *color;
    char *iface; /* yaml key: interface */
    bool enable;
    /* NULL == MT_GROUP_MODE_NORMAL, so every config written before this
     * field existed keeps its meaning. */
    char *mode;         /* yaml/json key: mode */
    char *on_exception; /* yaml/json key: onException */
    /* Route locally-destined traffic too. Deliberately phrased so that
     * absent (false, per the yaml contract) is the safe answer: an
     * except-group marks *everything*, and its routing table holds only a
     * default route plus a blackhole, so without this exemption the LAN
     * and the tunnel's own endpoint get routed into the tunnel and the
     * router stops forwarding. Opt in only for a deliberate
     * site-to-site setup. */
    bool route_local; /* yaml/json key: routeLocal */
    mt_rule_t **rules;
    size_t n_rules;
} mt_group_t;

/* True when the group routes everything *except* what its rules match. */
bool mt_group_is_except(const mt_group_t *g);
/* True when this except-group's excluded traffic must take the main route
 * instead of being offered to the remaining groups. Always false for a
 * normal group. */
bool mt_group_exception_is_terminal(const mt_group_t *g);

typedef struct mt_sub_rule {
    mt_id_t id;
    char *rule;
    char *type;
    bool enable;
} mt_sub_rule_t;

typedef struct mt_subscription {
    mt_id_t id;
    char *name;
    char *iface;
    bool enable;
    char *url;
    uint32_t interval;
    uint32_t last_update;
    uint32_t last_check; /* runtime only, never serialized */
    mt_sub_rule_t **rules;
    size_t n_rules;
} mt_subscription_t;

/* AppConfig with the exact defaults from Go constant.DefaultAppConfig. */
typedef struct mt_app_config {
    struct {
        bool enabled;
        struct {
            bool enabled;
        } auth;
        struct {
            char *address;
            uint16_t port;
        } host;
        char *skin;
    } http_web;
    struct {
        struct {
            char *address;
            uint16_t port;
        } host, upstream;
        bool disable_remap53;
        bool disable_fake_ptr;
        bool disable_drop_aaaa;
        uint64_t max_idle_conns;
        uint64_t max_concurrent;
        mt_duration_t timeout;
    } dns_proxy;
    struct {
        struct {
            char *chain_prefix;
        } iptables;
        struct {
            char *table_prefix;
            mt_duration_t additional_ttl;
        } ipset;
        bool disable_ipv4;
        bool disable_ipv6;
        uint32_t start_mark_table_index;
    } netfilter;
    char **link;
    size_t n_link;
    bool show_all_interfaces;
    char *log_level;
} mt_app_config_t;

/* Whole-application state produced by config load (defaults + overlay). */
typedef struct mt_config {
    mt_app_config_t app;
    mt_group_t **groups;
    size_t n_groups;
    bool groups_present; /* was the `groups` key present in YAML */
    mt_subscription_t **subscriptions;
    size_t n_subscriptions;
    bool subscriptions_present;
} mt_config_t;

mt_rule_t *mt_rule_new(void);
void mt_rule_free(mt_rule_t *r);

mt_group_t *mt_group_new(void);
void mt_group_free(mt_group_t *g);
mt_err_t mt_group_add_rule(mt_group_t *g, mt_rule_t *r); /* takes ownership */

mt_sub_rule_t *mt_sub_rule_new(void);
void mt_sub_rule_free(mt_sub_rule_t *r);

mt_subscription_t *mt_subscription_new(void);
void mt_subscription_free(mt_subscription_t *s);
mt_err_t mt_subscription_add_rule(mt_subscription_t *s, mt_sub_rule_t *r);

/* Initialize with DefaultAppConfig values (allocates strings). */
mt_err_t mt_app_config_init_defaults(mt_app_config_t *c);
void mt_app_config_clear(mt_app_config_t *c);

mt_err_t mt_config_init_defaults(mt_config_t *c);
void mt_config_clear(mt_config_t *c);
mt_err_t mt_config_add_group(mt_config_t *c, mt_group_t *g);
/* Frees c->groups[idx] and shifts the remaining pointers down (idx must
 * be < c->n_groups). */
void mt_config_remove_group_by_index(mt_config_t *c, size_t idx);
/* Frees every group and empties c->groups (does not touch subscriptions
 * or app config, unlike mt_config_clear). */
void mt_config_clear_groups(mt_config_t *c);
mt_err_t mt_config_add_subscription(mt_config_t *c, mt_subscription_t *s);
/* Frees c->subscriptions[idx] and shifts the remaining pointers down (idx
 * must be < c->n_subscriptions). */
void mt_config_remove_subscription_by_index(mt_config_t *c, size_t idx);
/* Frees every subscription and empties c->subscriptions (does not touch
 * groups or app config). */
void mt_config_clear_subscriptions(mt_config_t *c);

/* Group color normalization: valid #rrggbb (case-insensitive) is lowercased
 * in place; anything else becomes "#ffffff". Matches Go LoadConfig. */
mt_err_t mt_group_normalize_color(mt_group_t *g);

/* strdup that maps NULL input to NULL output and reports OOM. */
mt_err_t mt_strset(char **dst, const char *src);

#endif /* MAGITRICKLE_MODELS_H */
