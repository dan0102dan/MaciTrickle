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

#include "magitrickle/log.h"
#include "magitrickle/rulesnap.h"
#include "magitrickle/sub_runtime.h"

struct mt_app {
    mt_config_t *cfg;
    mt_cache_t *cache;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    mt_dns_pipeline_t *pipeline;
    uint32_t start_idx;
    bool running;

    mt_ruleset_t **rulesets;
    size_t n_rulesets;
    size_t cap_rulesets;

    /* Subscription rulesets: a separate array (mirrors Go's
     * a.subscriptionRuleSets being distinct from a.userRuleSets), since
     * they're rebuilt wholesale on every subscription change rather than
     * edited in place. sub_synth_groups[i] is the mt_group_t backing
     * sub_rulesets[i] -- mt_ruleset_t only *borrows* its group pointer
     * (ruleset.h), so unlike cfg->groups (long-lived, owned by the
     * config), this synthesized group must be kept alive here for
     * exactly as long as its ruleset exists, and freed alongside it. */
    mt_ruleset_t **sub_rulesets;
    mt_group_t **sub_synth_groups;
    size_t n_sub_rulesets;
    size_t cap_sub_rulesets;
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

static mt_err_t sub_rulesets_push(mt_app_t *app, mt_ruleset_t *rs, mt_group_t *synth) {
    if (app->n_sub_rulesets == app->cap_sub_rulesets) {
        size_t new_cap = app->cap_sub_rulesets ? app->cap_sub_rulesets * 2 : 8;
        mt_ruleset_t **nr = realloc(app->sub_rulesets, new_cap * sizeof(*nr));
        if (!nr) { return MT_ERR_NOMEM; }
        app->sub_rulesets = nr;
        mt_group_t **ng = realloc(app->sub_synth_groups, new_cap * sizeof(*ng));
        /* cap_sub_rulesets is only bumped once BOTH arrays have grown --
         * if ng fails, sub_rulesets is left secretly larger than
         * cap_sub_rulesets tracks, which is harmless (the next push just
         * redoes an equivalent-or-larger realloc; it never under-allocates
         * sub_synth_groups against a cap that outgrew it). */
        if (!ng) { return MT_ERR_NOMEM; }
        app->sub_synth_groups = ng;
        app->cap_sub_rulesets = new_cap;
    }
    app->sub_rulesets[app->n_sub_rulesets] = rs;
    app->sub_synth_groups[app->n_sub_rulesets] = synth;
    app->n_sub_rulesets++;
    return MT_OK;
}

static void free_subscription_rulesets(mt_app_t *app) {
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        mt_ruleset_disable(app->sub_rulesets[i]);
        mt_ruleset_free(app->sub_rulesets[i]);
        mt_group_free(app->sub_synth_groups[i]);
    }
    app->n_sub_rulesets = 0;
}

/* Disables+frees the whole current subscription-ruleset array and
 * rebuilds it from app->cfg->subscriptions -- matches Go's
 * syncSubscriptionRuleSetsLocked/buildSubscriptionRuleSetsLocked exactly:
 * always a full disable-then-rebuild, never an in-place edit. If
 * app->running, each new ruleset is enabled+synced; on failure, only
 * what THIS rebuild attempt already built is torn down (matching Go's
 * buildSubscriptionRuleSetsLocked, which disables what it just built and
 * returns the error without trying to restore any older state -- that's
 * the caller's job, see mt_app_add_subscription/etc.'s rollback). */
static mt_err_t rebuild_subscription_rulesets(mt_app_t *app) {
    free_subscription_rulesets(app);

    mt_ruleset_deps_t rdeps = ruleset_deps(app);
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        mt_group_t *synth = mt_sub_runtime_group(app->cfg->subscriptions[i]);
        if (!synth) {
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }
        mt_ruleset_t *rs = mt_ruleset_new(synth, &rdeps);
        if (!rs) {
            mt_group_free(synth);
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }
        if (sub_rulesets_push(app, rs, synth) != MT_OK) {
            mt_ruleset_free(rs);
            mt_group_free(synth);
            free_subscription_rulesets(app);
            return MT_ERR_NOMEM;
        }

        if (app->running) {
            mt_err_t err = mt_ruleset_enable(rs);
            if (err == MT_OK) { err = mt_ruleset_sync(rs, app->cache, (int64_t)time(NULL)); }
            if (err != MT_OK) {
                free_subscription_rulesets(app);
                return err;
            }
        }
    }
    return MT_OK;
}

static void republish_or_log(mt_app_t *app) {
    mt_err_t err = mt_app_republish_dns_snapshot(app);
    if (err != MT_OK) { MT_ERROR("failed to republish DNS-matching snapshot: %s", mt_err_str(err)); }
}

mt_err_t mt_app_republish_dns_snapshot(mt_app_t *app) {
    if (!app->pipeline) { return MT_OK; }
    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(app->cfg);
    if (!snap) { return MT_ERR_NOMEM; }
    mt_dns_pipeline_set_snapshot(app->pipeline, snap);
    return MT_OK;
}

mt_app_t *mt_app_create(const mt_app_deps_t *deps) {
    mt_app_t *app = calloc(1, sizeof(*app));
    if (!app) { return NULL; }
    app->cfg = deps->cfg;
    app->cache = deps->cache;
    app->ipt4 = deps->ipt4;
    app->ipt6 = deps->ipt6;
    app->rtnl = deps->rtnl;
    app->pipeline = deps->pipeline;
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
    /* Mirrors Go's LoadConfig calling syncSubscriptionRuleSetsLocked()
     * unconditionally at the end -- running is always false here (set
     * later via mt_app_set_running), so this only builds raw rulesets,
     * matching addGroupLocked's own enable/sync skip while !a.enabled. */
    if (rebuild_subscription_rulesets(app) != MT_OK) {
        mt_app_destroy(app);
        return NULL;
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
    free_subscription_rulesets(app);
    free(app->sub_rulesets);
    free(app->sub_synth_groups);
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
    republish_or_log(app);
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
    republish_or_log(app);
}

void mt_app_remove_group_by_index(mt_app_t *app, size_t idx) {
    if (idx >= app->n_rulesets) { return; }
    mt_ruleset_free(app->rulesets[idx]);
    rulesets_remove_at(app, idx);
    mt_config_remove_group_by_index(app->cfg, idx);
    republish_or_log(app);
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

size_t mt_app_subscription_ruleset_count(const mt_app_t *app) {
    return app->n_sub_rulesets;
}

mt_ruleset_t *mt_app_subscription_ruleset_at(const mt_app_t *app, size_t idx) {
    return idx < app->n_sub_rulesets ? app->sub_rulesets[idx] : NULL;
}

mt_ruleset_t *mt_app_find_subscription_ruleset_by_id(const mt_app_t *app, mt_id_t id) {
    for (size_t i = 0; i < app->n_sub_rulesets; i++) {
        if (mt_id_equal(mt_ruleset_group(app->sub_rulesets[i])->id, id)) { return app->sub_rulesets[i]; }
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
    if (err != MT_OK) {
        mt_subscription_free(sub);
        return err;
    }
    size_t idx = app->cfg->n_subscriptions - 1;

    err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        mt_config_remove_subscription_by_index(app->cfg, idx);
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_replace_subscriptions(mt_app_t *app, mt_subscription_t **subs, size_t n) {
    mt_subscription_t **new_arr = n > 0 ? calloc(n, sizeof(*new_arr)) : NULL;
    if (n > 0 && !new_arr) {
        for (size_t i = 0; i < n; i++) { mt_subscription_free(subs[i]); }
        free(subs);
        return MT_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) { new_arr[i] = subs[i]; }
    free(subs); /* array shell only; elements moved into new_arr */

    mt_subscription_t **old_arr = app->cfg->subscriptions;
    size_t old_n = app->cfg->n_subscriptions;
    app->cfg->subscriptions = new_arr;
    app->cfg->n_subscriptions = n;

    mt_err_t err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        for (size_t i = 0; i < n; i++) { mt_subscription_free(new_arr[i]); }
        free(new_arr);
        app->cfg->subscriptions = old_arr;
        app->cfg->n_subscriptions = old_n;
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }

    for (size_t i = 0; i < old_n; i++) { mt_subscription_free(old_arr[i]); }
    free(old_arr);
    republish_or_log(app);
    return MT_OK;
}

mt_err_t mt_app_remove_subscription_by_id(mt_app_t *app, mt_id_t id, bool *out_found) {
    size_t idx = app->cfg->n_subscriptions;
    for (size_t i = 0; i < app->cfg->n_subscriptions; i++) {
        if (mt_id_equal(app->cfg->subscriptions[i]->id, id)) {
            idx = i;
            break;
        }
    }
    if (idx == app->cfg->n_subscriptions) {
        *out_found = false;
        return MT_OK;
    }
    *out_found = true;

    mt_subscription_t *removed = app->cfg->subscriptions[idx];
    for (size_t i = idx; i + 1 < app->cfg->n_subscriptions; i++) {
        app->cfg->subscriptions[i] = app->cfg->subscriptions[i + 1];
    }
    app->cfg->n_subscriptions--;

    mt_err_t err = rebuild_subscription_rulesets(app);
    if (err != MT_OK) {
        /* Restore `removed` at its original index -- append then rotate
         * into place, matching Go's positional splice-back exactly. */
        mt_err_t insert_err = mt_config_add_subscription(app->cfg, removed);
        if (insert_err == MT_OK) {
            size_t last = app->cfg->n_subscriptions - 1;
            for (size_t i = last; i > idx; i--) {
                app->cfg->subscriptions[i] = app->cfg->subscriptions[i - 1];
            }
            app->cfg->subscriptions[idx] = removed;
        } else {
            MT_ERROR("failed to restore subscription after rollback: %s", mt_err_str(insert_err));
            mt_subscription_free(removed);
        }
        mt_err_t rollback_err = rebuild_subscription_rulesets(app);
        if (rollback_err != MT_OK) {
            MT_ERROR("failed to rollback subscription rulesets: %s", mt_err_str(rollback_err));
        }
        return err;
    }

    mt_subscription_free(removed);
    republish_or_log(app);
    return MT_OK;
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
