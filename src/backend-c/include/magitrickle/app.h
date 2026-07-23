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

#include "magitrickle/dns_cache.h"
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
 * a.recordsCache). */
typedef struct mt_app_deps {
    mt_config_t *cfg;
    mt_cache_t *cache;
    mt_ipt_t *ipt4;
    mt_ipt_t *ipt6;
    mt_rtnl_t *rtnl;
    uint32_t start_idx;
} mt_app_deps_t;

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
