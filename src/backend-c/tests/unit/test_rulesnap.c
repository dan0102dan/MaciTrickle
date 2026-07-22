#include "greatest.h"

#include "magitrickle/rulesnap.h"

static mt_config_t make_cfg(void)
{
    mt_config_t cfg;
    mt_config_init_defaults(&cfg);
    return cfg;
}

static mt_group_t *make_group(mt_id_t id, const char *name, bool enable)
{
    mt_group_t *g = mt_group_new();
    g->id = id;
    mt_strset(&g->name, name);
    g->enable = enable;
    return g;
}

static mt_rule_t *make_rule(mt_id_t id, const char *type, const char *rule,
                            bool enable)
{
    mt_rule_t *r = mt_rule_new();
    r->id = id;
    mt_strset(&r->type, type);
    mt_strset(&r->rule, rule);
    r->enable = enable;
    return r;
}

TEST disabled_group_excluded(void)
{
    mt_config_t cfg = make_cfg();
    mt_group_t *g1 = make_group((mt_id_t){{1, 0, 0, 0}}, "on", true);
    mt_group_add_rule(g1, make_rule((mt_id_t){{1, 1, 0, 0}}, "domain",
                                    "a.example.com", true));
    mt_group_t *g2 = make_group((mt_id_t){{2, 0, 0, 0}}, "off", false);
    mt_group_add_rule(g2, make_rule((mt_id_t){{2, 1, 0, 0}}, "domain",
                                    "b.example.com", true));
    mt_config_add_group(&cfg, g1);
    mt_config_add_group(&cfg, g2);

    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(&cfg);
    ASSERT(snap != NULL);
    ASSERT_EQ(1u, (unsigned)snap->n_groups);
    ASSERT_STR_EQ("on", snap->groups[0]->name);

    mt_ruleset_snapshot_free(snap);
    mt_config_clear(&cfg);
    PASS();
}

TEST disabled_rule_excluded_from_matcher(void)
{
    mt_config_t cfg = make_cfg();
    mt_group_t *g = make_group((mt_id_t){{1, 0, 0, 0}}, "g", true);
    mt_group_add_rule(g, make_rule((mt_id_t){{1, 1, 0, 0}}, "domain",
                                   "enabled.example.com", true));
    mt_group_add_rule(g, make_rule((mt_id_t){{1, 2, 0, 0}}, "domain",
                                   "disabled.example.com", false));
    mt_config_add_group(&cfg, g);

    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(&cfg);
    ASSERT_EQ(1u, (unsigned)snap->n_groups);
    ASSERT(mt_matcher_match(snap->groups[0]->matcher, "enabled.example.com"));
    ASSERT_FALSE(
        mt_matcher_match(snap->groups[0]->matcher, "disabled.example.com"));

    mt_ruleset_snapshot_free(snap);
    mt_config_clear(&cfg);
    PASS();
}

TEST multiple_groups_independently_match(void)
{
    /* an IP resolving into a name can route into more than one group,
     * mirroring Go's "no break at the group level" (decisions.md D-18) */
    mt_config_t cfg = make_cfg();
    mt_group_t *g1 = make_group((mt_id_t){{1, 0, 0, 0}}, "g1", true);
    mt_group_add_rule(
        g1, make_rule((mt_id_t){{1, 1, 0, 0}}, "namespace", "example.com",
                      true));
    mt_group_t *g2 = make_group((mt_id_t){{2, 0, 0, 0}}, "g2", true);
    mt_group_add_rule(
        g2, make_rule((mt_id_t){{2, 1, 0, 0}}, "wildcard", "*.example.com",
                      true));
    mt_config_add_group(&cfg, g1);
    mt_config_add_group(&cfg, g2);

    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(&cfg);
    ASSERT_EQ(2u, (unsigned)snap->n_groups);
    int matched = 0;
    for (size_t i = 0; i < snap->n_groups; i++) {
        if (mt_matcher_match(snap->groups[i]->matcher, "sub.example.com")) {
            matched++;
        }
    }
    ASSERT_EQ(2, matched);

    mt_ruleset_snapshot_free(snap);
    mt_config_clear(&cfg);
    PASS();
}

TEST empty_config_yields_empty_snapshot(void)
{
    mt_config_t cfg = make_cfg();
    mt_ruleset_snapshot_t *snap = mt_ruleset_snapshot_build(&cfg);
    ASSERT(snap != NULL);
    ASSERT_EQ(0u, (unsigned)snap->n_groups);
    mt_ruleset_snapshot_free(snap);
    mt_config_clear(&cfg);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(disabled_group_excluded);
    RUN_TEST(disabled_rule_excluded_from_matcher);
    RUN_TEST(multiple_groups_independently_match);
    RUN_TEST(empty_config_yields_empty_snapshot);
    GREATEST_MAIN_END();
}
