/* See app.h. Port of the group/interface/config-save slice of app.go. */
#include "magitrickle/app.h"

#include <errno.h>
#include <ifaddrs.h>
#include <linux/if.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct mt_app {
    mt_config_t *cfg;
    mt_cache_t *cache;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    uint32_t start_idx;
    bool running;

    mt_ruleset_t **rulesets;
    size_t n_rulesets;
    size_t cap_rulesets;
};

static mt_ruleset_deps_t ruleset_deps(mt_app_t *app) {
    mt_ruleset_deps_t deps = {
        .ipt4 = app->ipt4,
        .ipt6 = app->ipt6,
        .rtnl = app->rtnl,
        .ipset_prefix = app->cfg->app.netfilter.ipset.table_prefix,
        .chain_prefix = app->cfg->app.netfilter.iptables.chain_prefix,
        .start_idx = app->start_idx,
    };
    return deps;
}

static mt_err_t rulesets_push(mt_app_t *app, mt_ruleset_t *rs) {
    if (app->n_rulesets == app->cap_rulesets) {
        size_t new_cap = app->cap_rulesets ? app->cap_rulesets * 2 : 8;
        mt_ruleset_t **na = realloc(app->rulesets, new_cap * sizeof(*na));
        if (!na) { return MT_ERR_NOMEM; }
        app->rulesets = na;
        app->cap_rulesets = new_cap;
    }
    app->rulesets[app->n_rulesets++] = rs;
    return MT_OK;
}

static void rulesets_remove_at(mt_app_t *app, size_t idx) {
    for (size_t i = idx; i + 1 < app->n_rulesets; i++) {
        app->rulesets[i] = app->rulesets[i + 1];
    }
    app->n_rulesets--;
}

mt_app_t *mt_app_create(const mt_app_deps_t *deps) {
    mt_app_t *app = calloc(1, sizeof(*app));
    if (!app) { return NULL; }
    app->cfg = deps->cfg;
    app->cache = deps->cache;
    app->ipt4 = deps->ipt4;
    app->ipt6 = deps->ipt6;
    app->rtnl = deps->rtnl;
    app->start_idx = deps->start_idx;

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    for (size_t i = 0; i < app->cfg->n_groups; i++) {
        mt_ruleset_t *rs = mt_ruleset_new(app->cfg->groups[i], &rdeps);
        if (!rs || rulesets_push(app, rs) != MT_OK) {
            mt_ruleset_free(rs);
            mt_app_destroy(app);
            return NULL;
        }
    }
    return app;
}

void mt_app_destroy(mt_app_t *app) {
    if (!app) { return; }
    for (size_t i = 0; i < app->n_rulesets; i++) {
        mt_ruleset_disable(app->rulesets[i]);
        mt_ruleset_free(app->rulesets[i]);
    }
    free(app->rulesets);
    free(app);
}

void mt_app_set_running(mt_app_t *app, bool running) {
    app->running = running;
}

size_t mt_app_user_group_count(const mt_app_t *app) {
    return app->n_rulesets;
}

mt_ruleset_t *mt_app_user_group_at(const mt_app_t *app, size_t idx) {
    return idx < app->n_rulesets ? app->rulesets[idx] : NULL;
}

mt_ruleset_t *mt_app_find_group_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->rulesets[i])->id, id)) { return app->rulesets[i]; }
    }
    return NULL;
}

mt_err_t mt_app_add_group(mt_app_t *app, mt_group_t *group) {
    for (size_t i = 0; i < app->cfg->n_groups; i++) {
        if (mt_id_equal(app->cfg->groups[i]->id, group->id)) {
            mt_group_free(group);
            return MT_ERR_EXIST;
        }
    }
    for (size_t i = 0; i < group->n_rules; i++) {
        for (size_t j = i + 1; j < group->n_rules; j++) {
            if (mt_id_equal(group->rules[i]->id, group->rules[j]->id)) {
                mt_group_free(group);
                return MT_ERR_INVAL;
            }
        }
    }

    mt_err_t err = mt_config_add_group(app->cfg, group);
    if (err != MT_OK) {
        mt_group_free(group);
        return err;
    }
    size_t group_idx = app->cfg->n_groups - 1;

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    mt_ruleset_t *rs = mt_ruleset_new(app->cfg->groups[group_idx], &rdeps);
    if (!rs) {
        mt_config_remove_group_by_index(app->cfg, group_idx);
        return MT_ERR_NOMEM;
    }
    err = rulesets_push(app, rs);
    if (err != MT_OK) {
        mt_ruleset_free(rs);
        mt_config_remove_group_by_index(app->cfg, group_idx);
        return err;
    }

    if (app->running) {
        err = mt_ruleset_enable(rs);
        if (err == MT_OK) { err = mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL)); }
        if (err != MT_OK) {
            mt_ruleset_disable(rs);
            rulesets_remove_at(app, app->n_rulesets - 1);
            mt_ruleset_free(rs);
            mt_config_remove_group_by_index(app->cfg, group_idx);
            return err;
        }
    }
    return MT_OK;
}

mt_err_t mt_app_sync_group(mt_app_t *app, mt_ruleset_t *rs) {
    return mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL));
}

void mt_app_clear_groups(mt_app_t *app) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        mt_ruleset_disable(app->rulesets[i]);
        mt_ruleset_free(app->rulesets[i]);
    }
    app->n_rulesets = 0;
    mt_config_clear_groups(app->cfg);
}

void mt_app_remove_group_by_index(mt_app_t *app, size_t idx) {
    if (idx >= app->n_rulesets) { return; }
    mt_ruleset_free(app->rulesets[idx]);
    rulesets_remove_at(app, idx);
    mt_config_remove_group_by_index(app->cfg, idx);
}

bool mt_app_remove_group_by_id(mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->rulesets[i])->id, id)) {
            mt_app_remove_group_by_index(app, i);
            return true;
        }
    }
    return false;
}

/* ---- subscriptions ------------------------------------------------------------ */

size_t mt_app_subscription_count(const mt_app_t *app) {
    return app->cfg->n_subscriptions;
}

const mt_subscription_t *mt_app_subscription_at(const mt_app_t *app, size_t idx) {
    return idx < app->cfg->n_subscriptions ? app->cfg->subscriptions[idx] : NULL;
}

const mt_subscription_t *mt_app_find_subscription_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) { return app->cfg->subscriptions[i]; }
    }
    return NULL;
}

mt_err_t mt_app_add_subscription(mt_app_t *app, mt_subscription_t *sub) {
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, sub->id)) {
            mt_subscription_free(sub);
            return MT_ERR_EXIST;
        }
    }
    mt_err_t err = mt_config_add_subscription(app->cfg, sub);
    if (err != MT_OK) { mt_subscription_free(sub); }
    return err;
}

mt_err_t mt_app_replace_subscriptions(mt_app_t *app, mt_subscription_t **subs, size_t n) {
    mt_config_clear_subscriptions(app->cfg);
    mt_err_t err = MT_OK;
    size_t i = 0;
    for (; i < n; i++) {
        err = mt_config_add_subscription(app->cfg, subs[i]);
        if (err != MT_OK) { break; }
    }
    /* On an append failure (OOM), subs[i] and everything after it were
     * never absorbed into cfg->subscriptions -- free them here so nothing
     * leaks. */
    for (size_t j = err == MT_OK ? n : i; j < n; j++) { mt_subscription_free(subs[j]); }
    free(subs);
    return err;
}

bool mt_app_remove_subscription_by_id(mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) {
            mt_config_remove_subscription_by_index(app->cfg, i);
            return true;
        }
    }
    return false;
}

/* ---- interfaces -------------------------------------------------------------- */

mt_err_t mt_app_list_interfaces(const mt_app_t *app, mt_iface_info_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;

    struct ifaddrs *ifap;
    if (getifaddrs(&ifap) != 0) { return mt_err_from_errno(errno); }

    bool show_all = app->cfg->app.show_all_interfaces;
    size_t cap = 0;
    size_t n = 0;
    mt_iface_info_t *arr = NULL;

    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        bool already = false;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(arr[i].id, p->ifa_name) == 0) {
                already = true;
                break;
            }
        }
        if (already) { continue; }

        if (!show_all && (p->ifa_flags & IFF_POINTOPOINT) == 0) { continue; }

        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            mt_iface_info_t *na = realloc(arr, new_cap * sizeof(*na));
            if (!na) {
                free(arr);
                freeifaddrs(ifap);
                return MT_ERR_NOMEM;
            }
            arr = na;
            cap = new_cap;
        }
        memset(&arr[n], 0, sizeof(arr[n]));
        snprintf(arr[n].id, sizeof(arr[n].id), "%s", p->ifa_name);
        n++;
    }
    freeifaddrs(ifap);
    *out = arr;
    *out_n = n;
    return MT_OK;
}

/* ---- config save / iptables commit -------------------------------------------- */

mt_err_t mt_app_save_config(mt_app_t *app, const char *path, const char *version) {
    return mt_config_save_file(app->cfg, version, path);
}

mt_err_t mt_app_force_commit_iptables(mt_app_t *app) {
    if (app->ipt4) {
        mt_err_t err = mt_ipt_commit(app->ipt4);
        if (err != MT_OK) { return err; }
    }
    if (app->ipt6) {
        mt_err_t err = mt_ipt_commit(app->ipt6);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}
