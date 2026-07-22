// DNS differential oracle using miekg/dns (the exact library the Go backend
// uses). Reads hex-encoded DNS messages (one per line), and for each emits a
// canonical semantic dump that both sides can compare byte-for-byte.
//
// Commands:
//
//	dump       parse the message; print canonical form (or "PARSE_ERROR")
//	stripaaaa  parse, drop AAAA answers, re-pack with Compress=true, re-parse,
//	           print canonical form of the result (models the response hook)
//	ptrcheck   print "ptr" if the raw message is a single-PTR query, else "no"
//
// Canonical form is deterministic and compression-independent: header fields,
// then each section's RRs as "name TTL type rdata" with rdata rendered in a
// fixed textual form. This is the "semantic comparison after canonical parse"
// the migration spec (§21) requires.
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"sort"
	"strings"

	"github.com/miekg/dns"
)

func canonRR(rr dns.RR) string {
	h := rr.Header()
	typ := dns.TypeToString[h.Rrtype]
	if typ == "" {
		typ = fmt.Sprintf("TYPE%d", h.Rrtype)
	}
	var rdata string
	switch v := rr.(type) {
	case *dns.A:
		rdata = "A=" + v.A.String()
	case *dns.AAAA:
		rdata = "AAAA=" + v.AAAA.String()
	case *dns.CNAME:
		rdata = "CNAME=" + strings.ToLower(v.Target)
	case *dns.NS:
		rdata = "NS=" + strings.ToLower(v.Ns)
	case *dns.PTR:
		rdata = "PTR=" + strings.ToLower(v.Ptr)
	case *dns.MX:
		rdata = fmt.Sprintf("MX=%d,%s", v.Preference, strings.ToLower(v.Mx))
	case *dns.SOA:
		rdata = fmt.Sprintf("SOA=%s,%s,%d", strings.ToLower(v.Ns),
			strings.ToLower(v.Mbox), v.Serial)
	case *dns.OPT:
		rdata = "OPT"
	default:
		// generic: hex of rdata via wire
		buf := make([]byte, dns.MaxMsgSize)
		off, err := dns.PackRR(rr, buf, 0, nil, false)
		if err != nil {
			rdata = "RDATA?"
		} else {
			rdata = "RDATA=" + hex.EncodeToString(buf[:off])
		}
	}
	return fmt.Sprintf("%s %d %s %s", strings.ToLower(h.Name), h.Ttl, typ,
		rdata)
}

func canonMsg(m *dns.Msg) string {
	var b strings.Builder
	fmt.Fprintf(&b, "id=%d qr=%t opcode=%d rcode=%d ra=%t\n", m.Id,
		m.Response, m.Opcode, m.Rcode, m.RecursionAvailable)
	for _, q := range m.Question {
		typ := dns.TypeToString[q.Qtype]
		if typ == "" {
			typ = fmt.Sprintf("TYPE%d", q.Qtype)
		}
		fmt.Fprintf(&b, "Q %s %s %d\n", strings.ToLower(q.Name), typ,
			q.Qclass)
	}
	dumpSection := func(tag string, rrs []dns.RR) {
		lines := make([]string, 0, len(rrs))
		for _, rr := range rrs {
			if rr == nil {
				continue
			}
			lines = append(lines, tag+" "+canonRR(rr))
		}
		// answers stay in order; but OPT pseudo-records and ordering of
		// authority/additional across compression are stable in miekg —
		// keep insertion order for ANSWER, sort AUTH/ADDL to avoid
		// section-order noise
		if tag != "AN" {
			sort.Strings(lines)
		}
		for _, l := range lines {
			b.WriteString(l)
			b.WriteByte('\n')
		}
	}
	dumpSection("AN", m.Answer)
	dumpSection("NS", m.Ns)
	dumpSection("AR", m.Extra)
	return b.String()
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: dns_oracle dump|stripaaaa|ptrcheck")
		os.Exit(2)
	}
	cmd := os.Args[1]
	sc := bufio.NewScanner(os.Stdin)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()

	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		raw, err := hex.DecodeString(line)
		if err != nil {
			fmt.Fprintln(w, "HEX_ERROR")
			fmt.Fprintln(w, "===")
			continue
		}

		switch cmd {
		case "ptrcheck":
			m := new(dns.Msg)
			if err := m.Unpack(raw); err != nil {
				fmt.Fprintln(w, "PARSE_ERROR")
			} else if len(m.Question) == 1 &&
				m.Question[0].Qtype == dns.TypePTR {
				fmt.Fprintln(w, "ptr")
			} else {
				fmt.Fprintln(w, "no")
			}
		case "dump":
			m := new(dns.Msg)
			if err := m.Unpack(raw); err != nil {
				fmt.Fprintln(w, "PARSE_ERROR")
			} else {
				fmt.Fprint(w, canonMsg(m))
			}
		case "stripaaaa":
			m := new(dns.Msg)
			if err := m.Unpack(raw); err != nil {
				fmt.Fprintln(w, "PARSE_ERROR")
			} else {
				filtered := m.Answer[:0]
				for _, a := range m.Answer {
					if a.Header().Rrtype != dns.TypeAAAA {
						filtered = append(filtered, a)
					}
				}
				m.Answer = filtered
				m.Compress = true
				packed, err := m.Pack()
				if err != nil {
					fmt.Fprintln(w, "PACK_ERROR")
				} else {
					m2 := new(dns.Msg)
					if err := m2.Unpack(packed); err != nil {
						fmt.Fprintln(w, "REPARSE_ERROR")
					} else {
						fmt.Fprint(w, canonMsg(m2))
					}
				}
			}
		}
		fmt.Fprintln(w, "===")
	}
	_ = net.IPv4len
}
