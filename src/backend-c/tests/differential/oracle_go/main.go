// Go-side oracle for the differential suites. Uses the REAL backend code
// paths (magitrickle.App LoadConfig/SaveConfig, models.Rule.IsMatch,
// subscriptions.ParseRules) so the C implementation is compared against
// production behaviour, not a re-implementation.
//
// Commands:
//
//	resave <file>   copy file to the platform config path, App.New() +
//	                LoadConfig + SaveConfig, print the saved bytes
//	                ("ERROR" when the load fails). REQUIRES root (uses
//	                /var/lib/magitrickle) — CI and dev containers are root.
//	match           stdin: TYPE\tRULE\tDOMAIN lines -> ...\tmatch|nomatch
//	subparse        stdin: subscription list -> "type|rule|enable" lines
package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"

	"magitrickle"
	"magitrickle/models"
	"magitrickle/subscriptions"

	"github.com/rs/zerolog"
)

const cfgPath = "/var/lib/magitrickle/config.yaml"

func cmdResave(src string) {
	zerolog.SetGlobalLevel(zerolog.Disabled)

	if err := os.MkdirAll("/var/lib/magitrickle", 0o755); err != nil {
		fmt.Println("ERROR")
		return
	}
	_ = os.Remove(cfgPath)
	if src != "-missing-" {
		data, err := os.ReadFile(src)
		if err != nil {
			fmt.Println("ERROR")
			return
		}
		if err := os.WriteFile(cfgPath, data, 0o600); err != nil {
			fmt.Println("ERROR")
			return
		}
	}

	app := magitrickle.New() // logs (suppressed) on load error
	// Re-run LoadConfig explicitly to detect the error status.
	if err := app.LoadConfig(); err != nil {
		if src != "-missing-" {
			fmt.Println("ERROR")
			return
		}
	}
	if err := app.SaveConfig(); err != nil {
		fmt.Println("ERROR")
		return
	}
	out, err := os.ReadFile(cfgPath)
	if err != nil {
		fmt.Println("ERROR")
		return
	}
	_, _ = os.Stdout.Write(out)
}

func cmdMatch() {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1<<20), 1<<20)
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "\t", 3)
		if len(parts) != 3 {
			continue
		}
		rule := &models.Rule{Type: parts[0], Rule: parts[1]}
		res := "nomatch"
		if rule.IsMatch(parts[2]) {
			res = "match"
		}
		fmt.Fprintf(w, "%s\t%s\t%s\t%s\n", parts[0], parts[1], parts[2], res)
	}
}

func cmdSubparse() {
	data, err := io.ReadAll(os.Stdin)
	if err != nil {
		fmt.Println("ERROR")
		return
	}
	rules := subscriptions.ParseRules(string(data))
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	for _, r := range rules {
		fmt.Fprintf(w, "%s|%s|%v\n", r.Type, r.Rule, r.Enable)
	}
}

func main() {
	if len(os.Args) >= 3 && os.Args[1] == "resave" {
		cmdResave(os.Args[2])
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "match" {
		cmdMatch()
		return
	}
	if len(os.Args) == 2 && os.Args[1] == "subparse" {
		cmdSubparse()
		return
	}
	fmt.Fprintln(os.Stderr, "usage: oracle resave <file>|-missing- | match | subparse")
	os.Exit(2)
}
