// Emits the Go-yaml-v2 fixture for the yaml_emit spike: the same config
// data that emit_config.c produces with libyaml, marshalled through the
// exact structs the backend uses.
package main

import (
	"fmt"
	"time"

	"magitrickle/config"
	"magitrickle/models"
	"magitrickle/utils/intID"

	"go.yaml.in/yaml/v2"
)

func p[T any](v T) *T { return &v }

func mustID(s string) intID.ID {
	id, err := intID.ParseID(s)
	if err != nil {
		panic(err)
	}
	return id
}

func main() {
	groups := []*models.Group{{
		ID:        mustID("d663876a"),
		Name:      "Example",
		Color:     "#ffffff",
		Interface: "nwg0",
		Enable:    false,
		Rules: []*models.Rule{{
			ID:     mustID("6f34ee91"),
			Name:   "Wildcard Example",
			Type:   "wildcard",
			Rule:   "*wildcard.example.com",
			Enable: true,
		}},
	}}
	subs := []*models.Subscription{}

	cfg := config.Config{
		ConfigVersion: "0.99.0",
		App: &config.App{
			HTTPWeb: &config.HTTPWeb{
				Enabled: p(true),
				Auth:    &config.Auth{Enabled: p(false)},
				Host: &config.HTTPWebServer{
					Address: p("[::]"),
					Port:    p(uint16(8080)),
				},
				Skin: p("default"),
			},
			DNSProxy: &config.DNSProxy{
				Host: &config.DNSProxyServer{
					Address: p("[::]"),
					Port:    p(uint16(3553)),
				},
				Upstream: &config.DNSProxyServer{
					Address: p("127.0.0.1"),
					Port:    p(uint16(53)),
				},
				DisableRemap53:  p(false),
				DisableFakePTR:  p(false),
				DisableDropAAAA: p(false),
				MaxIdleConns:    p(uint(10)),
				MaxConcurrent:   p(uint(100)),
				Timeout:         p(5 * time.Second),
			},
			Netfilter: &config.Netfilter{
				IPTables: &config.IPTables{ChainPrefix: p("MT_")},
				IPSet: &config.IPSet{
					TablePrefix:   p("mt_"),
					AdditionalTTL: p(time.Hour),
				},
				DisableIPv4:         p(false),
				DisableIPv6:         p(false),
				StartMarkTableIndex: p(uint32(0x4D616769)),
			},
			Link:              p([]string{"br0"}),
			ShowAllInterfaces: p(false),
			LogLevel:          p("info"),
		},
		Groups:        &groups,
		Subscriptions: &subs,
	}

	out, err := yaml.Marshal(cfg)
	if err != nil {
		panic(err)
	}
	fmt.Print(string(out))
}
