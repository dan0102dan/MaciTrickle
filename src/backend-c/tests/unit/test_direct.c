/* "direct" lists: a group or subscription pointed at the reserved
 * interface routes nothing, and every routing group leaves its traffic
 * alone.
 *
 * Why this has to be a chain rule rather than just an absent chain: the DNS
 * path adds a resolved address to *every* group whose rules match the name.
 * A group with a catch-all pattern therefore also matches the domains a
 * direct list names, and without the leave-rules below it would route them
 * anyway -- the direct list would look configured and do nothing. */
#include "greatest.h"

#include <stdlib.h>
#include <string.h>

#include "fake_iptables.h"

#include "magitrickle/ipset_to_link.h"
#include "magitrickle/models.h"
#include "magitrickle/netfilter_cleaner.h"

typedef struct {
    mt_fake_ipt_t *fake;
    mt_ipt_t *ipt;
    mt_ipset_t *ipset;
    mt_ipset_to_link_t *link;
} fixture_t;

/* mt_ipset_t only needs its name for chain building, and a direct list
 * never allocates a mark, so neither the ipset transport nor rtnetlink is
 * touched here. */
static void fixture_up(fixture_t *fx, const char *iface, const char *const *bypass,
                      size_t n_bypass) {
    fx->fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    fx->ipt = mt_ipt_new(mt_fake_ipt_as_executable(fx->fake));
    mt_netfilter_register_base_chains(fx->ipt, NULL);

    fx->ipset = mt_ipset_new(NULL, "mt_g1");
    fx->link = mt_ipset_to_link_new("MT_g1", iface, fx->ipset, fx->ipt, NULL, NULL, 0);

    mt_ipset_to_link_mode_t mode = {.bypass_sets = bypass, .n_bypass_sets = n_bypass};
    mt_ipset_to_link_set_mode(fx->link, &mode);
}

static void fixture_down(fixture_t *fx) {
    mt_ipset_to_link_free(fx->link);
    mt_ipset_free(fx->ipset);
    mt_ipt_free(fx->ipt);
}

/* Renders one chain as "arg arg | arg arg" so a whole chain, order
 * included, can be asserted in a single compare. */
static void chain_text(mt_fake_ipt_t *f, const char *table, const char *chain, char *out,
                      size_t out_len) {
    mt_ipt_rule_t *const *rules = NULL;
    size_t n = 0;
    out[0] = '\0';
    if (!mt_fake_ipt_get_rules(f, table, chain, &rules, &n)) { return; }

    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && used + 3 < out_len) {
            memcpy(out + used, " | ", 3);
            used += 3;
        }
        for (size_t j = 0; j < rules[i]->n_parts; j++) {
            size_t len = strlen(rules[i]->parts[j]);
            if (j > 0 && used + 1 < out_len) { out[used++] = ' '; }
            if (used + len < out_len) {
                memcpy(out + used, rules[i]->parts[j], len);
                used += len;
            }
        }
        out[used] = '\0';
    }
}

static void build_and_commit(fixture_t *fx) {
    mt_ipset_to_link_force_enabled_for_test(fx->link, 1234);
    mt_ipset_to_link_prepare_iptables(fx->link);
    mt_ipt_commit(fx->ipt);
}

TEST direct_list_builds_no_chain(void) {
    fixture_t fx;
    fixture_up(&fx, MT_IPSET_TO_LINK_DIRECT, NULL, 0);

    /* enable() must not need netlink here: there is no mark to allocate and
     * no route to add, which is the whole point -- hence rtnl == NULL. */
    ASSERT_EQ(MT_OK, mt_ipset_to_link_enable(fx.link));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_prepare_iptables(fx.link));
    ASSERT_EQ(MT_OK, mt_ipt_commit(fx.ipt));

    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "mangle", "MT_g1"));
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "filter", "MT_g1"));
    ASSERT_FALSE(mt_fake_ipt_chain_exists(fx.fake, "nat", "MT_g1"));

    char buf[256];
    chain_text(fx.fake, "mangle", "PREROUTING", buf, sizeof(buf));
    ASSERT_STR_EQ("", buf);

    fixture_down(&fx);
    PASS();
}

TEST routing_group_returns_on_direct_sets_before_marking(void) {
    const char *bypass[] = {"mt_ru_direct", "mt_sub_whitelist"};
    fixture_t fx;
    fixture_up(&fx, "wg0", bypass, 2);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-m conntrack --ctdir REPLY -j RETURN"
                  " | -m set --match-set mt_ru_direct_4 dst -j RETURN"
                  " | -m set --match-set mt_sub_whitelist_4 dst -j RETURN"
                  " | -m set --match-set mt_g1_4 dst -j MARK --set-mark 1234"
                  " | -m set --match-set mt_g1_4 dst -j CONNMARK --save-mark",
                  buf);

    fixture_down(&fx);
    PASS();
}

/* The two-group setup this exists for: one direct list naming what to
 * ignore, one wildcard group taking everything else. */
TEST wildcard_group_still_skips_the_direct_list(void) {
    const char *bypass[] = {"mt_ru_direct"};
    fixture_t fx;
    fixture_up(&fx, "wg0", bypass, 1);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    const char *leave = strstr(buf, "mt_ru_direct_4 dst -j RETURN");
    const char *mark = strstr(buf, "-j MARK");
    ASSERT(leave != NULL);
    ASSERT(mark != NULL);
    /* Order is the whole guarantee: a leave-rule after the mark would never
     * be reached. */
    ASSERT(leave < mark);

    fixture_down(&fx);
    PASS();
}

TEST chain_is_unchanged_without_direct_lists(void) {
    fixture_t fx;
    fixture_up(&fx, "wg0", NULL, 0);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-m conntrack --ctdir REPLY -j RETURN"
                  " | -m set --match-set mt_g1_4 dst -j MARK --set-mark 1234"
                  " | -m set --match-set mt_g1_4 dst -j CONNMARK --save-mark",
                  buf);

    chain_text(fx.fake, "filter", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-o wg0 -m set --match-set mt_g1_4 dst -j ACCEPT", buf);
    chain_text(fx.fake, "nat", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-m set --match-set mt_g1_4 dst -j MASQUERADE", buf);

    fixture_down(&fx);
    PASS();
}

/* Re-setting the lists restages the chain, so a direct list appearing or
 * going away reaches the kernel on the next commit. */
TEST bypass_sets_can_be_replaced(void) {
    const char *first[] = {"mt_one"};
    fixture_t fx;
    fixture_up(&fx, "wg0", first, 1);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT(strstr(buf, "mt_one_4") != NULL);

    const char *second[] = {"mt_two", "mt_three"};
    mt_ipset_to_link_mode_t mode = {.bypass_sets = second, .n_bypass_sets = 2};
    ASSERT_EQ(MT_OK, mt_ipset_to_link_set_mode(fx.link, &mode));
    ASSERT_EQ(MT_OK, mt_ipset_to_link_prepare_iptables(fx.link));
    ASSERT_EQ(MT_OK, mt_ipt_commit(fx.ipt));

    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT(strstr(buf, "mt_one_4") == NULL);
    ASSERT(strstr(buf, "mt_two_4") != NULL);
    ASSERT(strstr(buf, "mt_three_4") != NULL);

    fixture_down(&fx);
    PASS();
}

TEST blackhole_still_behaves_as_before(void) {
    fixture_t fx;
    fixture_up(&fx, MT_IPSET_TO_LINK_BLACKHOLE, NULL, 0);
    build_and_commit(&fx);

    char buf[512];
    /* No filter ACCEPT for a blackhole group -- but it still marks, unlike a
     * direct list. */
    chain_text(fx.fake, "filter", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("", buf);
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT(strstr(buf, "-j MARK --set-mark 1234") != NULL);

    fixture_down(&fx);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(direct_list_builds_no_chain);
    RUN_TEST(routing_group_returns_on_direct_sets_before_marking);
    RUN_TEST(wildcard_group_still_skips_the_direct_list);
    RUN_TEST(chain_is_unchanged_without_direct_lists);
    RUN_TEST(bypass_sets_can_be_replaced);
    RUN_TEST(blackhole_still_behaves_as_before);
    GREATEST_MAIN_END();
}
