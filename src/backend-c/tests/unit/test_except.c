/* "Everything except" groups: the port-rule grammar, and the chain the
 * group compiles to. The property under test throughout is that the chain
 * evaluates NOT(exception1 OR exception2 OR ...) -- one leave-rule per
 * exception ahead of a single unconditional mark -- and never a negated
 * per-condition match. */
#include "greatest.h"

#include <stdlib.h>
#include <string.h>

#include "fake_iptables.h"

#include "magitrickle/ipset_to_link.h"
#include "magitrickle/models.h"
#include "magitrickle/netfilter_cleaner.h"
#include "magitrickle/portrule.h"

/* ---- port rule grammar ---------------------------------------------------- */

TEST port_rule_accepts_single_ports(void) {
    mt_port_rule_t r;
    ASSERT(mt_port_rule_parse("tcp/22", &r));
    ASSERT_STR_EQ("tcp", r.proto);
    ASSERT_EQ(22, r.lo);
    ASSERT_EQ(22, r.hi);

    ASSERT(mt_port_rule_parse("udp/53", &r));
    ASSERT_STR_EQ("udp", r.proto);
    ASSERT_EQ(53, r.lo);

    ASSERT(mt_port_rule_parse("TCP/443", &r));
    ASSERT_STR_EQ("tcp", r.proto);
    ASSERT_EQ(443, r.lo);
    PASS();
}

TEST port_rule_accepts_ranges(void) {
    mt_port_rule_t r;
    ASSERT(mt_port_rule_parse("tcp/1000-2000", &r));
    ASSERT_EQ(1000, r.lo);
    ASSERT_EQ(2000, r.hi);

    char buf[16];
    ASSERT_STR_EQ("1000:2000", mt_port_rule_dport(&r, buf, sizeof(buf)));

    ASSERT(mt_port_rule_parse("tcp/22", &r));
    ASSERT_STR_EQ("22", mt_port_rule_dport(&r, buf, sizeof(buf)));
    PASS();
}

TEST port_rule_rejects_nonsense(void) {
    mt_port_rule_t r;
    ASSERT_FALSE(mt_port_rule_parse("", &r));
    ASSERT_FALSE(mt_port_rule_parse("22", &r));            /* no protocol */
    ASSERT_FALSE(mt_port_rule_parse("sctp/22", &r));       /* unsupported */
    ASSERT_FALSE(mt_port_rule_parse("tcp/", &r));
    ASSERT_FALSE(mt_port_rule_parse("tcp/0", &r));         /* port 0 */
    ASSERT_FALSE(mt_port_rule_parse("tcp/65536", &r));     /* out of range */
    ASSERT_FALSE(mt_port_rule_parse("tcp/2000-1000", &r)); /* reversed */
    ASSERT_FALSE(mt_port_rule_parse("tcp/22x", &r));
    ASSERT_FALSE(mt_port_rule_parse("tcp/-22", &r));
    ASSERT_FALSE(mt_port_rule_parse("/22", &r));
    PASS();
}

/* ---- group mode accessors -------------------------------------------------- */

TEST group_defaults_to_normal(void) {
    mt_group_t *g = mt_group_new();
    ASSERT_FALSE(mt_group_is_except(g));
    ASSERT_FALSE(mt_group_exception_is_terminal(g));
    ASSERT_FALSE(mt_group_is_except(NULL));

    /* An explicit "normal" is still normal. */
    mt_strset(&g->mode, MT_GROUP_MODE_NORMAL);
    ASSERT_FALSE(mt_group_is_except(g));

    mt_strset(&g->mode, MT_GROUP_MODE_EXCEPT);
    ASSERT(mt_group_is_except(g));
    /* Absent onException means "continue", never terminal. */
    ASSERT_FALSE(mt_group_exception_is_terminal(g));

    mt_strset(&g->on_exception, MT_GROUP_ONEXC_MAINROUTE);
    ASSERT(mt_group_exception_is_terminal(g));

    /* A terminal exception on a normal group is meaningless. */
    mt_strset(&g->mode, MT_GROUP_MODE_NORMAL);
    ASSERT_FALSE(mt_group_exception_is_terminal(g));

    mt_group_free(g);
    PASS();
}

/* ---- the chain an except-group compiles to --------------------------------- */

typedef struct {
    mt_fake_ipt_t *fake;
    mt_ipt_t *ipt;
    mt_ipset_t *ipset;
    mt_ipset_to_link_t *link;
} chain_fixture_t;

/* mt_ipset_t only needs its name for chain building; the transport is never
 * touched because we never enable/add. */
static void chain_fixture_up(chain_fixture_t *fx, const mt_ipset_to_link_mode_t *mode) {
    fx->fake = mt_fake_ipt_new(MT_IPT_PROTO_IPV4);
    fx->ipt = mt_ipt_new(mt_fake_ipt_as_executable(fx->fake));
    mt_netfilter_register_base_chains(fx->ipt, NULL);

    fx->ipset = mt_ipset_new(NULL, "mt_g1");
    fx->link = mt_ipset_to_link_new("MT_g1", "wg0", fx->ipset, fx->ipt, NULL, NULL, 0);
    mt_ipset_to_link_set_mode(fx->link, mode);
}

static void chain_fixture_down(chain_fixture_t *fx) {
    mt_ipset_to_link_free(fx->link);
    mt_ipset_free(fx->ipset);
    mt_ipt_free(fx->ipt);
}

/* Renders one chain as "arg arg | arg arg" so a whole chain, order
 * included, can be asserted in one string compare. */
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

/* Drives the same builder the rebuild uses, with the link forced enabled,
 * and asserts the exact chain contents. */
static void build_and_commit(chain_fixture_t *fx) {
    mt_ipset_to_link_force_enabled_for_test(fx->link, 1234);
    mt_ipset_to_link_prepare_iptables(fx->link);
    mt_ipt_commit(fx->ipt);
}

TEST except_chain_shape(void) {
    mt_port_rule_t port;
    ASSERT(mt_port_rule_parse("tcp/22", &port));

    mt_ipset_to_link_mode_t mode = {
        .except = true, .route_local = true, .ports = &port, .n_ports = 1};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-m conntrack --ctdir REPLY -j RETURN"
                  " | -p tcp --dport 22 -j RETURN"
                  " | -m set --match-set mt_g1_4 dst -j RETURN"
                  " | -j MARK --set-mark 1234"
                  " | -j CONNMARK --save-mark",
                  buf);

    chain_fixture_down(&fx);
    PASS();
}

TEST except_chain_excludes_local_networks_by_default(void) {
    mt_ipset_to_link_mode_t mode = {.except = true};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[2048];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    /* Without these an except-group routes the LAN and the tunnel's own
     * endpoint into the tunnel. */
    ASSERT(strstr(buf, "-d 192.168.0.0/16 -j RETURN") != NULL);
    ASSERT(strstr(buf, "-d 10.0.0.0/8 -j RETURN") != NULL);
    ASSERT(strstr(buf, "-d 127.0.0.0/8 -j RETURN") != NULL);
    /* ... and they must come before the mark, or they would never be
     * reached. */
    ASSERT(strstr(buf, "-d 10.0.0.0/8 -j RETURN") < strstr(buf, "-j MARK"));

    chain_fixture_down(&fx);
    PASS();
}

TEST route_local_drops_the_local_carve_out(void) {
    mt_ipset_to_link_mode_t mode = {.except = true, .route_local = true};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[2048];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT(strstr(buf, "192.168.0.0/16") == NULL);

    chain_fixture_down(&fx);
    PASS();
}

TEST terminal_exception_uses_accept(void) {
    mt_ipset_to_link_mode_t mode = {
        .except = true, .terminal_exception = true, .route_local = true};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    /* ACCEPT ends the packet's trip through mangle, so no later group can
     * mark it: the exception really does take the main route. */
    ASSERT(strstr(buf, "-m set --match-set mt_g1_4 dst -j ACCEPT") != NULL);
    ASSERT(strstr(buf, "-j MARK --set-mark 1234") != NULL);

    chain_fixture_down(&fx);
    PASS();
}

TEST except_jump_goes_before_normal_groups(void) {
    mt_ipset_to_link_mode_t except_mode = {.except = true, .route_local = true};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &except_mode);

    /* A normal group sharing the same tables, staged after the except one. */
    mt_ipset_t *normal_set = mt_ipset_new(NULL, "mt_g2");
    mt_ipset_to_link_t *normal =
        mt_ipset_to_link_new("MT_g2", "eth0", normal_set, fx.ipt, NULL, NULL, 0);
    mt_ipset_to_link_mode_t normal_mode = {.except = false};
    mt_ipset_to_link_set_mode(normal, &normal_mode);

    build_and_commit(&fx);
    mt_ipset_to_link_force_enabled_for_test(normal, 5678);
    mt_ipset_to_link_prepare_iptables(normal);
    mt_ipt_commit(fx.ipt);

    char buf[512];
    chain_text(fx.fake, "mangle", "PREROUTING", buf, sizeof(buf));
    /* MARK is last-write-wins, so the catch-all must be traversed first for
     * a specific group to be able to override it. */
    ASSERT_STR_EQ("-j MT_g1 | -j MT_g2", buf);

    mt_ipset_to_link_free(normal);
    mt_ipset_free(normal_set);
    chain_fixture_down(&fx);
    PASS();
}

TEST normal_chain_is_unchanged(void) {
    mt_ipset_to_link_mode_t mode = {.except = false};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[1024];
    chain_text(fx.fake, "mangle", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-m conntrack --ctdir REPLY -j RETURN"
                  " | -m set --match-set mt_g1_4 dst -j MARK --set-mark 1234"
                  " | -m set --match-set mt_g1_4 dst -j CONNMARK --save-mark",
                  buf);

    chain_text(fx.fake, "filter", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-o wg0 -m set --match-set mt_g1_4 dst -j ACCEPT", buf);

    chain_fixture_down(&fx);
    PASS();
}

TEST except_filter_and_nat_match_the_interface(void) {
    mt_ipset_to_link_mode_t mode = {.except = true, .route_local = true};
    chain_fixture_t fx;
    chain_fixture_up(&fx, &mode);
    build_and_commit(&fx);

    char buf[512];
    chain_text(fx.fake, "filter", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-o wg0 -j ACCEPT", buf);
    chain_text(fx.fake, "nat", "MT_g1", buf, sizeof(buf));
    ASSERT_STR_EQ("-o wg0 -j MASQUERADE", buf);

    chain_fixture_down(&fx);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(port_rule_accepts_single_ports);
    RUN_TEST(port_rule_accepts_ranges);
    RUN_TEST(port_rule_rejects_nonsense);
    RUN_TEST(group_defaults_to_normal);
    RUN_TEST(except_chain_shape);
    RUN_TEST(except_chain_excludes_local_networks_by_default);
    RUN_TEST(route_local_drops_the_local_carve_out);
    RUN_TEST(terminal_exception_uses_accept);
    RUN_TEST(except_jump_goes_before_normal_groups);
    RUN_TEST(normal_chain_is_unchanged);
    RUN_TEST(except_filter_and_nat_match_the_interface);
    GREATEST_MAIN_END();
}
