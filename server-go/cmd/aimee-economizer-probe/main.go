// aimee-economizer-probe exposes the production economizer handler as a
// newline-delimited stdin/stdout process for reproducible benchmark runs.
//
// It deliberately has no provider or network access. Each input line is the
// exact economizer.ReduceRequest wire object; each output line is the exact
// economizer.ReduceResponse wire object returned by the production handler.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/economizer"
)

func main() {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 64*1024), 32*1024*1024)
	handler := economizer.NewHandler()
	encoder := json.NewEncoder(os.Stdout)

	for scanner.Scan() {
		line := scanner.Bytes()
		if !json.Valid(line) {
			fatal("input is not valid JSON")
		}
		out, status := handler(bus.ModuleInvocation{StageID: economizer.StageReduce}, line)
		if status != bus.ModuleStatusOK {
			fatal(fmt.Sprintf("economizer status %d", status))
		}
		var response json.RawMessage = out
		if err := encoder.Encode(response); err != nil {
			fatal(err.Error())
		}
	}
	if err := scanner.Err(); err != nil {
		fatal(err.Error())
	}
}

func fatal(message string) {
	fmt.Fprintln(os.Stderr, message)
	os.Exit(1)
}
