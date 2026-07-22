// Differential oracle for the DNS records cache. Imports the REAL
// production package (magitrickle/utils/recordsCache) unmodified — same
// non-invasive pattern as the Phase 2/3 oracles (config, DNS wire). The
// package's expiry sweep (cleanupRecords) is unexported, so instead of
// adding a test-only export to production code we drive it the same way
// production does: via the exported StartCleanup(ctx, interval) ticker,
// running at a high frequency for the lifetime of this short-lived
// process. The script's CLEANUP command just waits long enough for that
// ticker to have fired at least once since the last change.
//
// Reads a script from stdin, one command per line, and executes it against
// a live recordsCache.Records using real wall-clock time (SLEEP actually
// sleeps). This is compared against mt-cachetool running the identical
// script; because each side uses its OWN process's wall clock, the
// comparison is only ever structural (which addresses/aliases/domains are
// present) — never exact remaining-TTL seconds, which would be flaky
// across two independent processes.
//
// Commands:
//
//	ADDR domain ip ttl_seconds
//	ALIAS domain alias ttl_seconds
//	GETADDRS domain        -> "ADDRS domain: ip1,ip2,..." (sorted) or NONE
//	GETALIASES domain      -> "ALIASES domain: a1,a2,..." (sorted)
//	KNOWNDOMAINS           -> "KNOWN: d1,d2,..." (sorted)
//	CLEANUP                -> (no output; waits for the background sweep)
//	SLEEP ms               -> (no output; real sleep)
package main

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"magitrickle/utils/recordsCache"
)

const cleanupTick = 5 * time.Millisecond
const cleanupWait = 50 * time.Millisecond

func main() {
	r := recordsCache.New()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	r.StartCleanup(ctx, cleanupTick)

	sc := bufio.NewScanner(os.Stdin)
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		cmd := fields[0]

		switch cmd {
		case "ADDR":
			domain, ipStr, ttlStr := fields[1], fields[2], fields[3]
			ip := net.ParseIP(ipStr)
			if v4 := ip.To4(); v4 != nil {
				ip = v4
			}
			ttl, _ := strconv.ParseUint(ttlStr, 10, 32)
			r.AddAddress(domain, ip, uint32(ttl))

		case "ALIAS":
			domain, alias, ttlStr := fields[1], fields[2], fields[3]
			ttl, _ := strconv.ParseUint(ttlStr, 10, 32)
			r.AddAlias(domain, alias, uint32(ttl))

		case "GETADDRS":
			domain := fields[1]
			addrs := r.GetAddresses(domain)
			if addrs == nil {
				fmt.Fprintf(w, "ADDRS %s: NONE\n", domain)
				continue
			}
			ips := make([]string, 0, len(addrs))
			for _, a := range addrs {
				ips = append(ips, a.Address.String())
			}
			sort.Strings(ips)
			fmt.Fprintf(w, "ADDRS %s: %s\n", domain, strings.Join(ips, ","))

		case "GETALIASES":
			domain := fields[1]
			aliases := r.GetAliases(domain)
			sort.Strings(aliases)
			fmt.Fprintf(w, "ALIASES %s: %s\n", domain, strings.Join(aliases, ","))

		case "KNOWNDOMAINS":
			domains := r.ListKnownDomains()
			sort.Strings(domains)
			fmt.Fprintf(w, "KNOWN: %s\n", strings.Join(domains, ","))

		case "CLEANUP":
			time.Sleep(cleanupWait)

		case "SLEEP":
			ms, _ := strconv.Atoi(fields[1])
			time.Sleep(time.Duration(ms) * time.Millisecond)
		}
	}
}
