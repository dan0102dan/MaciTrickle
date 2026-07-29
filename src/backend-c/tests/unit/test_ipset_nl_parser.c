#include "greatest.h"

#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/netfilter/ipset/ip_set.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/ipset_nl_parser.h"

static struct nlmsghdr *list_message(uint8_t *buf, const uint8_t *addr, uint8_t iplen,
                                    uint8_t cidr, uint32_t timeout) {
    struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
    struct nfgenmsg *nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
    nfg->nfgen_family = AF_NETLINK;
    nfg->version = NFNETLINK_V0;
    nfg->res_id = 0;

    struct nlattr *adt = mnl_attr_nest_start(nlh, IPSET_ATTR_ADT);
    struct nlattr *data = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
    struct nlattr *ip = mnl_attr_nest_start(nlh, IPSET_ATTR_IP);
    uint16_t addr_type =
        (uint16_t)((iplen == 4 ? IPSET_ATTR_IPADDR_IPV4 : IPSET_ATTR_IPADDR_IPV6) |
                   NLA_F_NET_BYTEORDER);
    mnl_attr_put(nlh, addr_type, iplen, addr);
    mnl_attr_nest_end(nlh, ip);
    mnl_attr_put_u8(nlh, IPSET_ATTR_CIDR, cidr);
    mnl_attr_put_u32(nlh, IPSET_ATTR_TIMEOUT | NLA_F_NET_BYTEORDER, htonl(timeout));
    mnl_attr_nest_end(nlh, data);
    mnl_attr_nest_end(nlh, adt);
    return nlh;
}

TEST parses_nested_ipv4_entry(void) {
    uint8_t buf[512] = {0};
    const uint8_t addr[4] = {192, 0, 2, 128};
    struct nlmsghdr *nlh = list_message(buf, addr, 4, 25, 123);

    mt_ipset_entry4_t *out4 = NULL;
    mt_ipset_entry6_t *out6 = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_nl_parse_list_message(nlh, 4, &out4, &out6, &n));
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT(out4 != NULL);
    ASSERT(out6 == NULL);
    ASSERT_EQ(0, memcmp(addr, out4[0].subnet.addr, sizeof(addr)));
    ASSERT_EQ(25, out4[0].subnet.cidr);
    ASSERT(out4[0].has_timeout);
    ASSERT_EQ(123u, (unsigned)out4[0].timeout);

    free(out4);
    PASS();
}

TEST parses_nested_ipv6_entry(void) {
    uint8_t buf[512] = {0};
    const uint8_t addr[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                              0,    0,    0,    0,    0, 0, 0, 1};
    struct nlmsghdr *nlh = list_message(buf, addr, 16, 64, 0);

    mt_ipset_entry4_t *out4 = NULL;
    mt_ipset_entry6_t *out6 = NULL;
    size_t n = 0;
    ASSERT_EQ(MT_OK, mt_ipset_nl_parse_list_message(nlh, 16, &out4, &out6, &n));
    ASSERT_EQ(1u, (unsigned)n);
    ASSERT(out4 == NULL);
    ASSERT(out6 != NULL);
    ASSERT_EQ(0, memcmp(addr, out6[0].subnet.addr, sizeof(addr)));
    ASSERT_EQ(64, out6[0].subnet.cidr);
    /* Kernel timeout zero represents a permanent entry. */
    ASSERT_FALSE(out6[0].has_timeout);
    ASSERT_EQ(0u, (unsigned)out6[0].timeout);

    free(out6);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_nested_ipv4_entry);
    RUN_TEST(parses_nested_ipv6_entry);
    GREATEST_MAIN_END();
}
