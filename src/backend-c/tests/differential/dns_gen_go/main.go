// Generates a hex DNS-message corpus (valid, edge, and malformed) for the
// DNS differential suite. Valid messages are built with miekg/dns; malformed
// ones are hand-crafted byte strings. Output: one hex message per line.
package main

import (
	"encoding/hex"
	"fmt"
	"net"

	"github.com/miekg/dns"
)

func emit(m *dns.Msg) {
	packed, err := m.Pack()
	if err != nil {
		return
	}
	fmt.Println(hex.EncodeToString(packed))
}

func query(name string, qtype uint16) {
	m := new(dns.Msg)
	m.SetQuestion(dns.Fqdn(name), qtype)
	emit(m)
}

func main() {
	// queries
	query("example.com", dns.TypeA)
	query("example.com", dns.TypeAAAA)
	query("sub.example.com", dns.TypeA)
	query("1.0.0.127.in-addr.arpa", dns.TypePTR)
	query("a.very.long.chain.of.many.labels.example.com", dns.TypeA)
	query(".", dns.TypeNS)

	// A response
	{
		m := new(dns.Msg)
		m.SetQuestion("www.example.com.", dns.TypeA)
		m.Response = true
		m.RecursionAvailable = true
		m.Answer = append(m.Answer,
			&dns.A{Hdr: dns.RR_Header{Name: "www.example.com.",
				Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 300},
				A: net.ParseIP("93.184.216.34").To4()})
		emit(m)
		m.Compress = true
		emit(m)
	}

	// A + AAAA mixed (for stripaaaa)
	{
		m := new(dns.Msg)
		m.SetQuestion("dual.example.com.", dns.TypeA)
		m.Response = true
		m.Answer = append(m.Answer,
			&dns.A{Hdr: dns.RR_Header{Name: "dual.example.com.",
				Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 60},
				A: net.ParseIP("10.0.0.1").To4()},
			&dns.AAAA{Hdr: dns.RR_Header{Name: "dual.example.com.",
				Rrtype: dns.TypeAAAA, Class: dns.ClassINET, Ttl: 60},
				AAAA: net.ParseIP("2606:2800:220:1:248:1893:25c8:1946")},
			&dns.A{Hdr: dns.RR_Header{Name: "dual.example.com.",
				Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 60},
				A: net.ParseIP("10.0.0.2").To4()})
		m.Compress = true
		emit(m)
	}

	// CNAME chain response
	{
		m := new(dns.Msg)
		m.SetQuestion("alias.example.com.", dns.TypeA)
		m.Response = true
		m.Answer = append(m.Answer,
			&dns.CNAME{Hdr: dns.RR_Header{Name: "alias.example.com.",
				Rrtype: dns.TypeCNAME, Class: dns.ClassINET, Ttl: 120},
				Target: "target.example.net."},
			&dns.CNAME{Hdr: dns.RR_Header{Name: "target.example.net.",
				Rrtype: dns.TypeCNAME, Class: dns.ClassINET, Ttl: 120},
				Target: "final.example.org."},
			&dns.A{Hdr: dns.RR_Header{Name: "final.example.org.",
				Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 120},
				A: net.ParseIP("203.0.113.5").To4()})
		m.Compress = true
		emit(m)
	}

	// NXDOMAIN with SOA in authority
	{
		m := new(dns.Msg)
		m.SetQuestion("nope.example.com.", dns.TypeA)
		m.Response = true
		m.Rcode = dns.RcodeNameError
		m.Ns = append(m.Ns,
			&dns.SOA{Hdr: dns.RR_Header{Name: "example.com.",
				Rrtype: dns.TypeSOA, Class: dns.ClassINET, Ttl: 3600},
				Ns: "ns.example.com.", Mbox: "hostmaster.example.com.",
				Serial: 2024010101, Refresh: 7200, Retry: 3600,
				Expire: 1209600, Minttl: 3600})
		emit(m)
	}

	// MX + additional
	{
		m := new(dns.Msg)
		m.SetQuestion("example.com.", dns.TypeMX)
		m.Response = true
		m.Answer = append(m.Answer,
			&dns.MX{Hdr: dns.RR_Header{Name: "example.com.",
				Rrtype: dns.TypeMX, Class: dns.ClassINET, Ttl: 300},
				Preference: 10, Mx: "mail.example.com."})
		m.Extra = append(m.Extra,
			&dns.A{Hdr: dns.RR_Header{Name: "mail.example.com.",
				Rrtype: dns.TypeA, Class: dns.ClassINET, Ttl: 300},
				A: net.ParseIP("192.0.2.25").To4()})
		m.Compress = true
		emit(m)
	}

	// query with EDNS OPT
	{
		m := new(dns.Msg)
		m.SetQuestion("edns.example.com.", dns.TypeA)
		m.SetEdns0(4096, true)
		emit(m)
	}

	// malformed / adversarial (hand-crafted hex printed directly)
	malformed := []string{
		"",                             // empty
		"0000",                         // truncated header
		"000000000001000000000000",     // qdcount=1, no question
		"0000000000010000000000000377", // partial label
		// compression pointer loop (points to itself at offset 12)
		"000000000001000000000000c00c00010001", // ptr->12 self loop
		// name too long: many max labels
		"0000000000010000000000003f4141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141413f4242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242424242420000010001",
		// rdlength overflow
		"00000000000000010000000003777777076578616d706c6503636f6d0000010001000000ffff01",
		"81800001000000000000", // header only, counts lie
	}
	for _, h := range malformed {
		fmt.Println(h)
	}
}
