/* App layer — port of the group/interface/config-save slice of app.go's
 * App struct (compatibility-contract.md §2). Promotes what main.c
 * (Phase 5) built ad hoc into a reusable module the HTTP handlers
 * (Phase 6) can drive at runtime.
 *
 * Ownership model: mt_app_t does NOT own `cfg` (borrowed from main.c,
 * which loaded it before creating the app and keeps it alive for the
 * daemon's lifetime) but DOES treat cfg->groups as the live, mutable
 * group registry -- unlike Go, where a.userRuleSets (not config.Groups,
 * which doesn't even exist as an AppConfig field) is the runtime source
 * of truth and SaveConfig() reconstructs a fresh []*models.Group from
 * each RuleSet's Model() at save time. Because every mt_ruleset_t here
 * stores a `const mt_group_t *` pointing directly at the pointee inside
 * cfg->groups (an array of pointers, so appending/removing entries
 * reallocates the pointer array but never invalidates the pointees
 * themselves), keeping cfg->groups authoritative means mt_app_save_config
 * can just call the existing mt_config_save_file directly -- no
 * reconstruction step needed, unlike Go.
 *
 * Each mt_ruleset_t here also owns its own dedicated ipt4/ipt6/rtnl
 * *references* (borrowed from the shared deps, per decisions.md D-20).
 */
#ifndef MAGITRICKLE_APP_H
#define MAGITRICKLE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magitrickle/dns_cache.h"
#include "magitrickle/dnspipeline.h"
#include "magitrickle/err.h"
#include "magitrickle/id.h"
#include "magitrickle/iptables.h"
#include "magitrickle/models.h"
#include "magitrickle/rtnl.h"
#include "magitrickle/ruleset.h"
#include "magitrickle/yamlio.h"

typedef struct mt_app mt_app_t;

/* All pointers borrowed; must outlive the app. ipt4/ipt6/rtnl/start_idx
 * are passed straight through to every mt_ruleset_t the app creates
 * (mirrors mt_ruleset_deps_t); cache backs mt_app_add_group's initial
 * Sync() call for a group added while the app is already running
 * (matches Go's addGroupLocked calling grp.Sync(), which reads
 * a.recordsCache). pipeline (may be NULL, e.g. in unit tests that don't
 * care about DNS matching) is republished (mt_dns_pipeline_set_snapshot
 * with a freshly built mt_ruleset_snapshot_build(cfg)) after every group
 * or subscription mutation this module performs, and must be called by
 * the HTTP handlers too after any in-place rule edit that doesn't go
 * through one of this module's mutation functions -- see
 * mt_app_republish_dns_snapshot. */
typedef struct mt_app_deps {
    mt_config_t *cfg;
    mt_cache_t *cache;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    mt_dns_pipeline_t *pipeline;
    uint32_t start_idx;
} mt_app_deps_t;

/* Rebuilds and republishes the DNS-matching snapshot (mt_ruleset_snapshot_build
 * over the live cfg, including both groups and subscriptions) onto
 * deps->pipeline. No-op if pipeline is NULL (e.g. unit tests). Called
 * internally after every group/subscription mutation this module
 * performs; the HTTP handlers (groups.c) must call this explicitly too
 * after mutating a group's rules/fields in place via
 * mt_ruleset_group_mut (a path that bypasses this module's own mutation
 * functions) -- this was a real gap found in Phase 7 (see decisions.md):
 * Phase 6's group/rule HTTP handlers updated netfilter state via
 * mt_app_sync_group but never refreshed DNS matching, so an
 * HTTP-created group's domains would never actually resolve into it. */
mt_err_t mt_app_republish_dns_snapshot(mt_app_t *app);

mt_app_t *mt_app_create(const mt_app_deps_t *deps);
/* Disables and frees every group's mt_ruleset_t (mirrors main.c's
 * previous inline teardown); does NOT touch cfg (borrowed). */
void mt_app_destroy(mt_app_t *app);

/* Marks the app "running" -- mirrors Go's App.enabled atomic.Bool: once
 * true, mt_app_add_group immediately Enable()s+Sync()s the new group
 * (matching Go's addGroupLocked). Call this after main.c's own startup
 * loop has already Enable()+Sync()'d the initial group set directly (Go:
 * the CAS in Start() happens before that same loop) -- so the initial
 * groups are brought up by main.c, not through this path. */
void mt_app_set_running(mt_app_t *app, bool running);

size_t mt_app_user_group_count(const mt_app_t *app);
mt_ruleset_t *mt_app_user_group_at(const mt_app_t *app, size_t idx);
/* Linear search (group counts are small; matches the O(n) scans Go's
 * own loops over a.userRuleSets already are). NULL if not found. */
mt_ruleset_t *mt_app_find_group_by_id(const mt_app_t *app, mt_id_t id);

/* Takes ownership of `group` on any outcome: appends it to cfg->groups
 * (mt_config_add_group) and wraps it in a new mt_ruleset_t on success;
 * frees it and returns an error otherwise. Errors: MT_ERR_EXIST (group ID
 * already present, matches Go's ErrGroupIDConflict), MT_ERR_INVAL
 * (duplicate rule ID within the group, matches ErrRuleIDConflict), or
 * whatever mt_ruleset_enable/mt_ruleset_sync returns when the app is
 * already running (the partially-added group is rolled back: disabled,
 * unwrapped, and cfg->groups is shrunk back down -- matches Go's
 * removeAdded closure). */
mt_err_t mt_app_add_group(mt_app_t *app, mt_group_t *group);

/* Re-syncs a single already-registered ruleset using the app's shared cache
 * and current time -- used by the group/rule HTTP handlers (Phase 6) after
 * editing an enabled group's rules, mirroring Go's RuleSet.Sync() (which
 * reaches a.recordsCache through the RuleSet's owning *App). rs must be one
 * of app's own rulesets (e.g. from mt_app_find_group_by_id). */
mt_err_t mt_app_sync_group(mt_app_t *app, mt_ruleset_t *rs);

/* Disables every group (matches Go's ClearGroups, which -- unlike
 * RemoveGroupByIndex/RemoveGroupByID below -- does call Disable() itself)
 * then frees every mt_ruleset_t and empties cfg->groups. */
void mt_app_clear_groups(mt_app_t *app);
/* No Disable() call (matches Go exactly: RemoveGroupByIndex/ByID are
 * "dumb" splices -- callers, i.e. the HTTP handlers in Phase 6, disable
 * the group themselves first when it's enabled, exactly like
 * api/v1/handlers.go's DeleteGroup does). Frees the mt_ruleset_t and
 * removes the matching cfg->groups entry. */
void mt_app_remove_group_by_index(mt_app_t *app, size_t idx);
bool mt_app_remove_group_by_id(mt_app_t *app, mt_id_t id);

/* ---- subscriptions -------------------------------------------------------------
 *
 * Each configured subscription now gets a runtime mt_ruleset_t too (Phase
 * 7), built from a synthesized mt_group_t (mt_sub_runtime_group, mirroring
 * Go's subscriptionAsRuntimeRuleSet) -- kept in a *separate* array from
 * the group rulesets (mirrors Go's a.subscriptionRuleSets being a
 * distinct slice from a.userRuleSets), since a subscription's runtime
 * object needs to be entirely rebuilt (disable+free+recreate) on every
 * subscription-list or subscription-rules change, unlike a group's
 * ruleset which is edited in place. All three mutators below rebuild
 * the *entire* subscription-ruleset array after mutating
 * cfg->subscriptions -- matches Go's syncSubscriptionRuleSetsLocked,
 * which always disables+drops every subscription RuleSet and rebuilds
 * from scratch, never edits one in place -- and roll back to the
 * pre-mutation cfg->subscriptions state if the rebuild fails while the
 * app is running (matches Go's rollback-and-resync-old-state pattern in
 * AddSubscription/ReplaceSubscriptions/RemoveSubscriptionByID). Also
 * republishes the DNS-matching snapshot on any successful outcome (see
 * mt_app_republish_dns_snapshot). */

size_t mt_app_subscription_count(const mt_app_t *app);
const mt_subscription_t *mt_app_subscription_at(const mt_app_t *app, size_t idx);
/* Linear search; NULL if not found. */
const mt_subscription_t *mt_app_find_subscription_by_id(const mt_app_t *app, mt_id_t id);

/* Runtime ruleset iteration/lookup -- mirrors mt_app_user_group_at/
 * mt_app_find_group_by_id, but over the separate subscription-ruleset
 * array. Used by main.c's netlink-watcher dispatch (a subscription's
 * interface can go up/change address too, matching Go's handleLink/
 * handleAddr iterating ruleSetSnapshot() = groups+subscriptions) and by
 * the DNS match sink (a matched snapshot entry's id may belong to either
 * a group or a subscription). */
size_t mt_app_subscription_ruleset_count(const mt_app_t *app);
mt_ruleset_t *mt_app_subscription_ruleset_at(const mt_app_t *app, size_t idx);
mt_ruleset_t *mt_app_find_subscription_ruleset_by_id(const mt_app_t *app, mt_id_t id);

/* Takes ownership of `sub` on any outcome: appends to cfg->subscriptions,
 * rebuilds the subscription-ruleset array, and republishes the DNS
 * snapshot on success; frees `sub` and returns MT_ERR_EXIST on an ID
 * conflict (matches Go's app.ErrSubscriptionConflict -> HTTP 409)
 * without attempting a rebuild. If the app is running and the rebuild
 * fails (e.g. mt_ruleset_enable/sync failure for the new subscription,
 * or any other configured subscription -- Go rebuilds the *entire* set
 * every time, so one bad entry can affect all), `sub` is removed from
 * cfg->subscriptions again and the rulesets are rebuilt from the
 * pre-add state before returning the error. */
mt_err_t mt_app_add_subscription(mt_app_t *app, mt_subscription_t *sub);

/* Wholesale replace: takes ownership of `subs` (the array and every
 * element) on any outcome, discarding the previous cfg->subscriptions.
 * On rebuild failure while running, cfg->subscriptions is restored to
 * its pre-call contents (deep copies taken before the swap) and the
 * rulesets rebuilt from that -- matches Go's ReplaceSubscriptions
 * rollback exactly (including logging-then-continuing if the *rollback
 * rebuild itself* also fails, since there is nothing further to revert
 * to). */
mt_err_t mt_app_replace_subscriptions(mt_app_t *app, mt_subscription_t **subs, size_t n);

/* Removes the subscription, rebuilds the subscription-ruleset array, and
 * republishes the DNS snapshot. Returns false if not found (no rebuild
 * attempted). Matches Go's RemoveSubscriptionByID: on a running app with
 * a rebuild failure, the subscription is restored (matches Go's splice
 * back to its original index) and the rulesets rebuilt again from that
 * restored state; the function still returns true (the removal was
 * requested and is what the caller should report) alongside the error. */
mt_err_t mt_app_remove_subscription_by_id(mt_app_t *app, mt_id_t id, bool *out_found);

/* ---- subscription sync (fetch-backed) --------------------------------------
 *
 * Ports of subscriptions.go's SyncSubscriptionByID/SyncDueSubscriptions:
 * fetch the list over the network (mt_sub_fetch_list), diff it against the
 * subscription's current rules (mt_sub_refresh_rules/mt_sub_same_rules),
 * and -- only if the rules actually changed -- rebuild the subscription
 * ruleset array (rollback to the pre-sync state on failure, exactly like
 * the other subscription mutators above).
 *
 * Both perform a *blocking* network fetch on the calling thread. In this
 * port that thread is always the single event-loop thread (HTTP handlers
 * and the auto-update timer both run there -- see decisions.md D-33): a
 * slow or hanging upstream server stalls DNS resolution and HTTP serving
 * for up to MT_SUB_FETCH_TIMEOUT_SECONDS. D-02/D-17 already flagged a
 * worker-thread + mt_loop_post redesign as future work; scoped out here
 * given the size of the httpd.c refactor (deferred responses) it would
 * require. */

/* Looks up the subscription, fetches url_override (or the subscription's
 * own URL if url_override is NULL/empty), refreshes its rules, and
 * rebuilds the subscription-ruleset array + republishes the DNS snapshot
 * if the rules changed. On MT_OK, *out_changed reports whether the URL
 * and/or rules actually changed (matches Go's returned bool); the caller
 * should re-fetch the subscription via mt_app_find_subscription_by_id to
 * read its post-sync url/rules/last_update (no separate copy is handed
 * back -- safe because nothing else can run between this call returning
 * and the caller's next statement in this single-threaded model).
 *
 * Errors: MT_ERR_NOENT (no such subscription id), MT_ERR_INVAL (no URL
 * available -- empty override and empty subscription URL), MT_ERR_UPSTREAM
 * (the fetch failed -- any reason; see the log for detail), or whatever
 * the ruleset rebuild returned (rollback already attempted; a rollback
 * failure is logged, not returned separately -- matches
 * mt_app_add_subscription's pattern). On any failure after a successful
 * fetch where rules had changed, *out_changed is still set to true
 * (matches Go's literal `true` return in that branch -- "a change was
 * attempted", even though it was rolled back). */
mt_err_t mt_app_sync_subscription_by_id(mt_app_t *app, mt_id_t id, int64_t now_unix,
                                        const char *url_override, bool *out_changed);

/* Fetches every subscription for which mt_sub_is_due() is true (matches
 * IsDue's own semantics), applies PlanRefresh to each, and -- if at least
 * one subscription's rules actually changed -- rebuilds the subscription-
 * ruleset array once for the whole batch and republishes the DNS
 * snapshot (matches Go: LastCheck is bumped for every subscription that
 * was successfully fetched, even if unchanged, but a rebuild only
 * happens when something changed). A fetch failure for one subscription
 * is logged and that subscription is skipped -- it does not fail the
 * whole call (matches Go's continue-on-fetch-error loop). On a rebuild
 * failure, every subscription touched by this call is rolled back to its
 * pre-call state and the rulesets rebuilt again from that (rollback
 * failure logged, not returned). *out_any_changed mirrors Go's returned
 * bool, including staying true on a rolled-back failure (see above).
 * Unlike mt_app_sync_subscription_by_id, this does NOT call
 * mt_app_save_config itself (it doesn't have a path/version) -- matches
 * Go structurally in spirit only; the caller (main.c's auto-update timer,
 * wired in a later Phase 7 task) must save on *out_any_changed == true,
 * exactly like the maybe_save() pattern already used by the HTTP
 * handlers. */
mt_err_t mt_app_sync_due_subscriptions(mt_app_t *app, int64_t now_unix, bool *out_any_changed);

typedef struct mt_iface_info {
    char id[16];   /* IFNAMSIZ */
    char name[64]; /* friendly alias; empty unless a router-specific API
                    * supplies one -- always empty here, matching Go's
                    * DummyRouterSpecificAPI (the Keenetic RCI alias
                    * lookup is entware_kn-only and deferred to Phase 8) */
} mt_iface_info_t;

/* Enumerates system network interfaces (getifaddrs, deduped by name),
 * matching Go's interfaces.List: when cfg->app.show_all_interfaces is
 * false, keeps only interfaces with IFF_POINTOPOINT set (and not in the
 * platform's ignored-interfaces list -- empty on the default/non-
 * entware_kn platform, so a no-op here today). Caller frees *out. */
mt_err_t mt_app_list_interfaces(const mt_app_t *app, mt_iface_info_t **out, size_t *out_n);

/* Writes the current config (cfg, already kept live-authoritative for
 * groups per the header comment above) to its file path. */
mt_err_t mt_app_save_config(mt_app_t *app, const char *path, const char *version);

/* Commits ipt4/ipt6 (whichever are non-NULL) -- matches
 * App.ForceCommitIPTables, called from the netfilterd webhook. */
mt_err_t mt_app_force_commit_iptables(mt_app_t *app);

#endif /* MAGITRICKLE_APP_H */
