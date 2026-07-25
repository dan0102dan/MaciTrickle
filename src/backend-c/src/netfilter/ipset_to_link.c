/* See ipset_to_link.h. Port of utils/netfilterTools/ipset-to-link.go. */
#include "magitrickle/ipset_to_link.h"
#include "magitrickle/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct family_state {
    bool rule_added;
    bool blackhole_added;
    bool iface_route_present;
    bool iface_has_gw;
    uint8_t gw[16];
    uint8_t gw_len;
} family_state_t;

struct mt_ipset_to_link {
    char *chain_name;
    char *iface_name;
    mt_ipset_t *ipset; /* borrowed */
    mt_ipt_t *ipt4;    /* borrowed, nullable */
    mt_ipt_t *ipt6;    /* borrowed, nullable */
    mt_rtnl_t *rtnl;   /* borrowed */
    uint32_t start_idx;

    bool enabled;
    uint32_t mark;
    uint32_t table;
    family_state_t v4, v6;

    /* Chain shape; see mt_ipset_to_link_set_mode. */
    bool except;
    bool terminal_exception;
    bool route_local;
    mt_port_rule_t *ports; /* owned copy */
    size_t n_ports;
    char **bypass_sets; /* owned copies of base names */
    size_t n_bypass_sets;
};

/* True when this group only contributes an ipset for others to bypass. */
static bool is_direct(const mt_ipset_to_link_t *l) {
    return strcmp(l->iface_name, MT_IPSET_TO_LINK_DIRECT) == 0;
}

/* Destinations an except-group must never pull into its interface unless
 * asked to. Its routing table holds only a default route and a blackhole,
 * so marking these would send the LAN, the loopback and the link-local
 * traffic into the tunnel -- and, with the tunnel endpoint itself usually
 * living in one of these ranges, would route the tunnel's own packets into
 * the tunnel. Kept as explicit chain rules rather than set entries so they
 * are visible in `iptables -S` when someone is working out why a packet
 * went where it did. */
static const char *const k_local_v4[] = {
    "0.0.0.0/8",      /* this host */
    "10.0.0.0/8",     "172.16.0.0/12", "192.168.0.0/16", /* RFC1918 */
    "100.64.0.0/10",                                     /* CGNAT (RFC6598) */
    "127.0.0.0/8",                                       /* loopback */
    "169.254.0.0/16",                                    /* link-local */
    "224.0.0.0/4",                                       /* multicast */
    "255.255.255.255/32",                                /* broadcast */
};

static const char *const k_local_v6[] = {
    "::1/128",   /* loopback */
    "fe80::/10", /* link-local */
    "fc00::/7",  /* unique-local */
    "ff00::/8",  /* multicast */
};

mt_ipset_to_link_t *mt_ipset_to_link_new(const char *chain_name, const char *iface_name,
                                         mt_ipset_t *ipset, mt_ipt_t *ipt4, mt_ipt_t *ipt6,
                                         mt_rtnl_t *rtnl, uint32_t start_idx) {
    mt_ipset_to_link_t *l = calloc(1, sizeof(*l));
    if (!l) { return NULL; }
    l->chain_name = strdup(chain_name);
    l->iface_name = strdup(iface_name);
    if (!l->chain_name || !l->iface_name) {
        free(l->chain_name);
        free(l->iface_name);
        free(l);
        return NULL;
    }
    l->ipset = ipset;
    l->ipt4 = ipt4;
    l->ipt6 = ipt6;
    l->rtnl = rtnl;
    l->start_idx = start_idx;
    return l;
}

void mt_ipset_to_link_free(mt_ipset_to_link_t *l) {
    if (!l) { return; }
    free(l->chain_name);
    free(l->iface_name);
    free(l->ports);
    for (size_t i = 0; i < l->n_bypass_sets; i++) { free(l->bypass_sets[i]); }
    free(l->bypass_sets);
    free(l);
}

void mt_ipset_to_link_force_enabled_for_test(mt_ipset_to_link_t *l, uint32_t mark) {
    if (!l) { return; }
    l->enabled = true;
    l->mark = mark;
    l->table = mark;
}

mt_err_t mt_ipset_to_link_set_mode(mt_ipset_to_link_t *l, const mt_ipset_to_link_mode_t *mode) {
    if (!l || !mode) { return MT_ERR_INVAL; }

    mt_port_rule_t *ports = NULL;
    if (mode->n_ports > 0) {
        ports = calloc(mode->n_ports, sizeof(*ports));
        if (!ports) { return MT_ERR_NOMEM; }
        memcpy(ports, mode->ports, mode->n_ports * sizeof(*ports));
    }

    char **bypass = NULL;
    size_t n_bypass = 0;
    if (mode->n_bypass_sets > 0) {
        bypass = calloc(mode->n_bypass_sets, sizeof(*bypass));
        if (!bypass) {
            free(ports);
            return MT_ERR_NOMEM;
        }
        for (; n_bypass < mode->n_bypass_sets; n_bypass++) {
            bypass[n_bypass] = strdup(mode->bypass_sets[n_bypass]);
            if (!bypass[n_bypass]) {
                for (size_t i = 0; i < n_bypass; i++) { free(bypass[i]); }
                free(bypass);
                free(ports);
                return MT_ERR_NOMEM;
            }
        }
    }

    free(l->ports);
    l->ports = ports;
    l->n_ports = mode->n_ports;
    for (size_t i = 0; i < l->n_bypass_sets; i++) { free(l->bypass_sets[i]); }
    free(l->bypass_sets);
    l->bypass_sets = bypass;
    l->n_bypass_sets = n_bypass;
    l->except = mode->except;
    l->terminal_exception = mode->terminal_exception;
    l->route_local = mode->route_local;
    return MT_OK;
}

/* ---- iptables chain rules ------------------------------------------------ */

/* Emits the rules that carve the exceptions out of an except-group's mangle
 * chain: the port conditions, the local networks unless the group opted
 * into them, and the group's own exception set. Whatever survives them the
 * caller marks. Order among them does not matter -- together they are one
 * OR, and any single hit is enough. */
static mt_err_t build_exception_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt, const char *ipset_name,
                                      bool v6) {
    /* ACCEPT ends the packet's journey through mangle entirely, so no later
     * group can mark it and it leaves on the main route. RETURN ends only
     * our own chain, leaving the remaining groups their say. */
    const char *verdict = l->terminal_exception ? "ACCEPT" : "RETURN";
    mt_err_t err;

    for (size_t i = 0; i < l->n_ports; i++) {
        char dport[16];
        mt_port_rule_dport(&l->ports[i], dport, sizeof(dport));
        const char *args[] = {"-p", l->ports[i].proto, "--dport", dport, "-j", verdict};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, args, 6);
        if (err != MT_OK) { return err; }
    }

    if (!l->route_local) {
        const char *const *nets = v6 ? k_local_v6 : k_local_v4;
        size_t n_nets = v6 ? sizeof(k_local_v6) / sizeof(k_local_v6[0])
                           : sizeof(k_local_v4) / sizeof(k_local_v4[0]);
        for (size_t i = 0; i < n_nets; i++) {
            const char *args[] = {"-d", nets[i], "-j", verdict};
            err = mt_ipt_append(ipt, "mangle", l->chain_name, args, 4);
            if (err != MT_OK) { return err; }
        }
    }

    /* Every "direct" list, then this group's own exceptions. All of them
     * are one OR: whichever hits first, the packet is left alone. */
    for (size_t i = 0; i < l->n_bypass_sets; i++) {
        char set_name[256];
        snprintf(set_name, sizeof(set_name), "%s%s", l->bypass_sets[i], v6 ? "_6" : "_4");
        const char *args[] = {"-m", "set", "--match-set", set_name, "dst", "-j", verdict};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, args, 7);
        if (err != MT_OK) { return err; }
    }

    const char *set_args[] = {"-m", "set", "--match-set", ipset_name, "dst", "-j", verdict};
    return mt_ipt_append(ipt, "mangle", l->chain_name, set_args, 7);
}

/* Stages the group's chains and rules without writing them: a full table
 * rebuild stages every group first and writes the result in one commit.  */
static mt_err_t build_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt, const char *ipset_name) {
    if (!ipt) { return MT_OK; }
    /* A direct group is only a set for others to bypass -- it marks
     * nothing, so it needs no chain of its own. */
    if (is_direct(l)) { return MT_OK; }

    const bool v6 = mt_ipt_proto(ipt) == MT_IPT_PROTO_IPV6;
    mt_err_t err = mt_ipt_register_chain_override(ipt, "filter", l->chain_name);
    if (err != MT_OK) { return err; }

    if (strcmp(l->iface_name, MT_IPSET_TO_LINK_BLACKHOLE) != 0) {
        if (l->except) {
            /* Nothing reaches this interface unless our mangle chain marked
             * it, so the outgoing interface is the whole condition here --
             * no need to restate the exceptions. */
            const char *args[] = {"-o", l->iface_name, "-j", "ACCEPT"};
            err = mt_ipt_append(ipt, "filter", l->chain_name, args, 4);
        } else {
            const char *args[] = {"-o", l->iface_name, "-m", "set", "--match-set",
                                  ipset_name,          "dst", "-j", "ACCEPT"};
            err = mt_ipt_append(ipt, "filter", l->chain_name, args, 9);
        }
        if (err != MT_OK) { return err; }
    }
    const char *fwd_args[] = {"-j", l->chain_name};
    err = mt_ipt_append(ipt, "filter", "FORWARD", fwd_args, 2);
    if (err != MT_OK) { return err; }

    err = mt_ipt_register_chain_override(ipt, "mangle", l->chain_name);
    if (err != MT_OK) { return err; }

    char mark_str[16];
    snprintf(mark_str, sizeof(mark_str), "%u", l->mark);
    const char *mangle1[] = {"-m", "conntrack", "--ctdir", "REPLY", "-j", "RETURN"};
    err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle1, 6);
    if (err != MT_OK) { return err; }
    if (l->except) {
        /* NOT(exception1 OR exception2 OR ...): leave on any exception,
         * then mark whatever survived them. */
        err = build_exception_rules(l, ipt, ipset_name, v6);
        if (err != MT_OK) { return err; }

        const char *mark_args[] = {"-j", "MARK", "--set-mark", mark_str};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, mark_args, 4);
        if (err != MT_OK) { return err; }
        /* Without this rule, routing on Keenetic routers did not work; DO NOT REMOVE! */
        const char *save_args[] = {"-j", "CONNMARK", "--save-mark"};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, save_args, 3);
        if (err != MT_OK) { return err; }
    } else {
        const char *mangle2[] = {"-m", "set", "--match-set", ipset_name,
                                 "dst", "-j", "MARK",        "--set-mark", mark_str};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle2, 9);
        if (err != MT_OK) { return err; }
        /* Without this rule, routing on Keenetic routers did not work; DO NOT REMOVE! */
        const char *mangle3[] = {"-m", "set", "--match-set", ipset_name,
                                 "dst", "-j", "CONNMARK",    "--save-mark"};
        err = mt_ipt_append(ipt, "mangle", l->chain_name, mangle3, 8);
        if (err != MT_OK) { return err; }
    }

    const char *pre_args[] = {"-j", l->chain_name};
    if (l->except) {
        /* An except-group's jump goes first so the ordinary, more specific
         * groups appended after it overwrite its mark -- MARK is
         * last-write-wins, so "everything except X through wg0" never
         * overrides an explicit "example.com through WAN". */
        err = mt_ipt_insert(ipt, "mangle", "PREROUTING", 1, pre_args, 2);
    } else {
        err = mt_ipt_append(ipt, "mangle", "PREROUTING", pre_args, 2);
    }
    if (err != MT_OK) { return err; }

    err = mt_ipt_register_chain_override(ipt, "nat", l->chain_name);
    if (err != MT_OK) { return err; }
    if (l->except) {
        /* Same reasoning as the filter rule: only traffic this group marked
         * ever leaves via its interface. */
        const char *nat1[] = {"-o", l->iface_name, "-j", "MASQUERADE"};
        err = mt_ipt_append(ipt, "nat", l->chain_name, nat1, 4);
    } else {
        const char *nat1[] = {"-m", "set", "--match-set", ipset_name, "dst", "-j", "MASQUERADE"};
        err = mt_ipt_append(ipt, "nat", l->chain_name, nat1, 7);
    }
    if (err != MT_OK) { return err; }
    const char *post_args[] = {"-j", l->chain_name};
    err = mt_ipt_append(ipt, "nat", "POSTROUTING", post_args, 2);
    if (err != MT_OK) { return err; }

    return MT_OK;
}

static mt_err_t insert_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt, const char *ipset_name) {
    if (!ipt) { return MT_OK; }

    mt_err_t err = build_iptables_rules(l, ipt, ipset_name);
    if (err != MT_OK) { return err; }
    return mt_ipt_commit(ipt);
}

mt_err_t mt_ipset_to_link_prepare_iptables(mt_ipset_to_link_t *l) {
    if (!l || !l->enabled) { return MT_OK; }

    mt_err_t e4 = build_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
    mt_err_t e6 = build_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset));
    return e4 != MT_OK ? e4 : e6;
}

static mt_err_t delete_iptables_rules(mt_ipset_to_link_t *l, mt_ipt_t *ipt) {
    if (!ipt) { return MT_OK; }
    mt_err_t first_err = MT_OK;

    mt_err_t err = mt_ipt_register_chain_delete(ipt, "filter", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *fwd_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "filter", "FORWARD", fwd_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_register_chain_delete(ipt, "mangle", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *pre_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "mangle", "PREROUTING", pre_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_register_chain_delete(ipt, "nat", l->chain_name);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    const char *post_args[] = {"-j", l->chain_name};
    err = mt_ipt_delete(ipt, "nat", "POSTROUTING", post_args, 2);
    if (err != MT_OK && err != MT_ERR_NOENT && first_err == MT_OK) { first_err = err; }

    err = mt_ipt_commit(ipt);
    if (err != MT_OK && first_err == MT_OK) { first_err = err; }
    return first_err;
}

/* ---- ip rule ---------------------------------------------------------- */

static mt_err_t insert_ip_rule(mt_ipset_to_link_t *l) {
    /* The best-effort delete before each add mirrors Go's
     * insertIPRule(): `_ = netlink.RuleDel(rule)` (error deliberately
     * ignored) before `netlink.RuleAdd(rule)`. RTM_NEWRULE carries
     * NLM_F_EXCL, so without it a leftover rule with the same mark/table
     * -- e.g. from a previous daemon instance killed before it could
     * clean up -- makes every subsequent enable fail with EEXIST
     * ("already exists") instead of being taken over. */
    if (l->ipt4) {
        (void)mt_rtnl_rule_del(l->rtnl, AF_INET, l->mark, l->table);
        mt_err_t err = mt_rtnl_rule_add(l->rtnl, AF_INET, l->mark, l->table);
        if (err != MT_OK) { return err; }
        l->v4.rule_added = true;
    }
    if (l->ipt6) {
        (void)mt_rtnl_rule_del(l->rtnl, AF_INET6, l->mark, l->table);
        mt_err_t err = mt_rtnl_rule_add(l->rtnl, AF_INET6, l->mark, l->table);
        if (err != MT_OK) { return err; }
        l->v6.rule_added = true;
    }
    return MT_OK;
}

static mt_err_t delete_ip_rule(mt_ipset_to_link_t *l) {
    mt_err_t first_err = MT_OK;
    if (l->v4.rule_added) {
        mt_err_t err = mt_rtnl_rule_del(l->rtnl, AF_INET, l->mark, l->table);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v4.rule_added = false;
    }
    if (l->v6.rule_added) {
        mt_err_t err = mt_rtnl_rule_del(l->rtnl, AF_INET6, l->mark, l->table);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v6.rule_added = false;
    }
    return first_err;
}

/* ---- ip route ----------------------------------------------------------- */

static mt_err_t update_iface_route(mt_ipset_to_link_t *l, int family, int ifindex,
                                   bool point_to_point, family_state_t *fs) {
    bool has_gw = false;
    uint8_t gw[16] = {0};
    uint8_t gw_len = 0;

    if (!point_to_point) {
        bool found;
        mt_err_t err = mt_rtnl_gateway_for_iface(l->rtnl, family, ifindex, &found, gw, &gw_len);
        if (err != MT_OK) {
            MT_WARN("gateway lookup failed for %s: %s", l->iface_name, mt_err_str(err));
        } else if (found) {
            has_gw = true;
        }
    }

    if (fs->iface_route_present) {
        bool same_gw = has_gw == fs->iface_has_gw &&
                       (!has_gw || (gw_len == fs->gw_len && memcmp(gw, fs->gw, gw_len) == 0));
        if (same_gw) { return MT_OK; }
        if (has_gw) {
            mt_err_t err = mt_rtnl_route_del_iface(l->rtnl, family, l->table, 10, ifindex,
                                                   fs->iface_has_gw ? fs->gw : NULL, fs->gw_len);
            if (err != MT_OK) { return err; }
            fs->iface_route_present = false;
        }
    }

    bool enodev = false;
    mt_err_t err = mt_rtnl_route_add_iface(l->rtnl, family, l->table, 10, ifindex,
                                          has_gw ? gw : NULL, gw_len, &enodev);
    if (err != MT_OK) { return err; }
    if (enodev) {
        MT_WARN("interface %s not ready for this IP family, skipping route", l->iface_name);
        fs->iface_route_present = false;
        return MT_OK;
    }

    fs->iface_route_present = true;
    fs->iface_has_gw = has_gw;
    fs->gw_len = gw_len;
    memcpy(fs->gw, gw, sizeof(fs->gw));
    return MT_OK;
}

static mt_err_t insert_ip_route(mt_ipset_to_link_t *l) {
    if (l->ipt4) {
        mt_err_t err = mt_rtnl_route_add_blackhole(l->rtnl, AF_INET, l->table, 20);
        if (err != MT_OK) { return err; }
        l->v4.blackhole_added = true;
    }
    if (l->ipt6) {
        mt_err_t err = mt_rtnl_route_add_blackhole(l->rtnl, AF_INET6, l->table, 20);
        if (err != MT_OK) { return err; }
        l->v6.blackhole_added = true;
    }

    if (strcmp(l->iface_name, MT_IPSET_TO_LINK_BLACKHOLE) == 0) { return MT_OK; }

    mt_link_info_t li;
    bool found;
    mt_err_t err = mt_rtnl_link_by_name(l->rtnl, l->iface_name, &li, &found);
    if (err != MT_OK) { return err; }
    if (!found) {
        MT_WARN("interface %s not found, it can be caught later", l->iface_name);
        return MT_OK;
    }
    if (!li.up) {
        MT_WARN("interface %s is down", l->iface_name);
        return MT_OK;
    }

    if (l->ipt4) {
        err = update_iface_route(l, AF_INET, li.ifindex, li.point_to_point, &l->v4);
        if (err != MT_OK) { return err; }
    }
    if (l->ipt6) {
        err = update_iface_route(l, AF_INET6, li.ifindex, li.point_to_point, &l->v6);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}

static mt_err_t delete_ip_route(mt_ipset_to_link_t *l) {
    mt_err_t first_err = MT_OK;

    if (l->v4.iface_route_present) {
        mt_link_info_t li;
        bool found;
        mt_rtnl_link_by_name(l->rtnl, l->iface_name, &li, &found);
        int ifindex = found ? li.ifindex : 0;
        mt_err_t err = mt_rtnl_route_del_iface(l->rtnl, AF_INET, l->table, 10, ifindex,
                                              l->v4.iface_has_gw ? l->v4.gw : NULL, l->v4.gw_len);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v4.iface_route_present = false;
    }
    if (l->v6.iface_route_present) {
        mt_link_info_t li;
        bool found;
        mt_rtnl_link_by_name(l->rtnl, l->iface_name, &li, &found);
        int ifindex = found ? li.ifindex : 0;
        mt_err_t err = mt_rtnl_route_del_iface(l->rtnl, AF_INET6, l->table, 10, ifindex,
                                              l->v6.iface_has_gw ? l->v6.gw : NULL, l->v6.gw_len);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v6.iface_route_present = false;
    }
    if (l->v4.blackhole_added) {
        mt_err_t err = mt_rtnl_route_del_blackhole(l->rtnl, AF_INET, l->table, 20);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v4.blackhole_added = false;
    }
    if (l->v6.blackhole_added) {
        mt_err_t err = mt_rtnl_route_del_blackhole(l->rtnl, AF_INET6, l->table, 20);
        if (err != MT_OK && first_err == MT_OK) { first_err = err; }
        l->v6.blackhole_added = false;
    }
    return first_err;
}

/* ---- public API --------------------------------------------------------- */

static mt_err_t teardown(mt_ipset_to_link_t *l) {
    mt_err_t e1 = delete_ip_route(l);
    mt_err_t e2 = delete_ip_rule(l);
    mt_err_t e3 = delete_iptables_rules(l, l->ipt4);
    mt_err_t e4 = delete_iptables_rules(l, l->ipt6);
    if (e1 != MT_OK) { return e1; }
    if (e2 != MT_OK) { return e2; }
    if (e3 != MT_OK) { return e3; }
    return e4;
}

mt_err_t mt_ipset_to_link_enable(mt_ipset_to_link_t *l) {
    if (l->enabled) { return MT_OK; }

    /* A direct list routes nothing, so it needs no mark, no routing table,
     * no ip rule and no chain -- just the ipset, which mt_ruleset_t has
     * already created. Claiming a mark for it would waste a table and, more
     * importantly, imply a route that must never exist. */
    if (is_direct(l)) {
        l->enabled = true;
        MT_DEBUG("chain %s is a direct list: no mark, no route", l->chain_name);
        return MT_OK;
    }

    uint32_t idx;
    mt_err_t err = mt_rtnl_alloc_mark_table(l->rtnl, l->start_idx, &idx);
    if (err != MT_OK) { return err; }
    l->mark = idx;
    l->table = idx;

    err = insert_ip_rule(l);
    if (err == MT_OK) { err = insert_ip_route(l); }
    if (err == MT_OK) {
        err = insert_iptables_rules(l, l->ipt4, mt_ipset_name4(l->ipset));
    }
    if (err == MT_OK) {
        err = insert_iptables_rules(l, l->ipt6, mt_ipset_name6(l->ipset));
    }

    if (err != MT_OK) {
        teardown(l);
        return err;
    }

    l->enabled = true;
    MT_DEBUG("using ip table and mark 0x%x for chain %s", l->mark, l->chain_name);
    return MT_OK;
}

mt_err_t mt_ipset_to_link_disable(mt_ipset_to_link_t *l) {
    if (!l->enabled) { return MT_OK; }
    l->enabled = false;
    return teardown(l);
}

mt_err_t mt_ipset_to_link_clear_if_disabled(mt_ipset_to_link_t *l) {
    if (l->enabled) { return MT_OK; }
    return teardown(l);
}

mt_err_t mt_ipset_to_link_on_link_up(mt_ipset_to_link_t *l) {
    if (!l->enabled) { return MT_OK; }
    return insert_ip_route(l);
}

mt_err_t mt_ipset_to_link_on_addr_change(mt_ipset_to_link_t *l) {
    if (!l->enabled || strcmp(l->iface_name, MT_IPSET_TO_LINK_BLACKHOLE) == 0) { return MT_OK; }

    mt_link_info_t li;
    bool found;
    mt_err_t err = mt_rtnl_link_by_name(l->rtnl, l->iface_name, &li, &found);
    if (err != MT_OK) { return err; }
    if (!found) { return MT_OK; }

    if (l->ipt4) {
        err = update_iface_route(l, AF_INET, li.ifindex, li.point_to_point, &l->v4);
        if (err != MT_OK) { return err; }
    }
    if (l->ipt6) {
        err = update_iface_route(l, AF_INET6, li.ifindex, li.point_to_point, &l->v6);
        if (err != MT_OK) { return err; }
    }
    return MT_OK;
}
