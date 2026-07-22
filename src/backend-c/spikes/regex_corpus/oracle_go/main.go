// Oracle: evaluates the corpus with dlclark/regexp2 exactly as the Go
// backend does (regexp2.Compile with IgnoreCase, unanchored MatchString).
// Output: PATTERN\tINPUT\tRESULT where RESULT is match|nomatch|compile_error.
package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"

	"github.com/dlclark/regexp2"
)

func main() {
	scanner := bufio.NewScanner(os.Stdin)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()

	for scanner.Scan() {
		line := scanner.Text()
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "\t", 2)
		pattern := parts[0]
		input := ""
		if len(parts) == 2 {
			input = parts[1]
		}

		re, err := regexp2.Compile(pattern, regexp2.IgnoreCase)
		if err != nil {
			fmt.Fprintf(out, "%s\t%s\tcompile_error\n", pattern, input)
			continue
		}
		ok, err := re.MatchString(input)
		if err != nil {
			fmt.Fprintf(out, "%s\t%s\tmatch_error\n", pattern, input)
			continue
		}
		if ok {
			fmt.Fprintf(out, "%s\t%s\tmatch\n", pattern, input)
		} else {
			fmt.Fprintf(out, "%s\t%s\tnomatch\n", pattern, input)
		}
	}
}
