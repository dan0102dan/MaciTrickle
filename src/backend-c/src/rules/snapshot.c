#include "magitrickle/rulesnap.h"

#include <stdlib.h>
#include <string.h>

static void group_snapshot_free(mt_group_snapshot_t *g)
{
    if (g == NULL) {
        return;
    }
    free(g->name);
    mt_matcher_free(g->matcher);
    free(g);
}

mt_ruleset_snapshot_t *mt_ruleset_snapshot_build(const mt_config_t *cfg)
{
    mt_ruleset_snapshot_t *snap = calloc(1, sizeof(*snap));
    if (snap == NULL) {
        return NULL;
    }
    if (cfg->n_groups == 0) {
        return snap;
    }
    snap->groups = calloc(cfg->n_groups, sizeof(mt_group_snapshot_t *));
    if (snap->groups == NULL) {
        free(snap);
        return NULL;
    }

    for (size_t i = 0; i < cfg->n_groups; i++) {
        const mt_group_t *g = cfg->groups[i];
        if (!g->enable) {
            continue; /* disabled groups can never emit an action */
        }
        mt_group_snapshot_t *gs = calloc(1, sizeof(*gs));
        if (gs == NULL) {
            mt_ruleset_snapshot_free(snap);
            return NULL;
        }
        gs->id = g->id;
        gs->name = strdup(g->name != NULL ? g->name : "");
        gs->matcher = mt_matcher_new();
        if (gs->name == NULL || gs->matcher == NULL) {
            group_snapshot_free(gs);
            mt_ruleset_snapshot_free(snap);
            return NULL;
        }
        for (size_t j = 0; j < g->n_rules; j++) {
            const mt_rule_t *r = g->rules[j];
            if (!r->enable) {
                continue;
            }
            if (mt_matcher_add(gs->matcher, r->type, r->rule) != MT_OK) {
                group_snapshot_free(gs);
                mt_ruleset_snapshot_free(snap);
                return NULL;
            }
        }
        snap->groups[snap->n_groups++] = gs;
    }
    return snap;
}

void mt_ruleset_snapshot_free(mt_ruleset_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    for (size_t i = 0; i < snap->n_groups; i++) {
        group_snapshot_free(snap->groups[i]);
    }
    free(snap->groups);
    free(snap);
}
