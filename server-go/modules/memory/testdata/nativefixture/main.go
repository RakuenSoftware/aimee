// Native compatibility tests call the real Go memory owner. This executable is
// test-only: its trusted host invocation is never exposed on a network endpoint.
package main

import (
	"bufio"
	"encoding/json"
	"os"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
)

func main() {
	handler := memory.NewHandler(nil, memory.WithDataStore(memory.PlacementServer, nil))
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 65536), 2<<20)
	encoder := json.NewEncoder(os.Stdout)
	for scanner.Scan() {
		frame, err := bus.EncodeCommand("runtime", scanner.Bytes())
		if err != nil {
			os.Exit(1)
		}
		encoded, status := handler(bus.ModuleInvocation{StageID: memory.StageCommand}, frame)
		body, err := bus.DecodeCommandResult(encoded)
		if status != bus.ModuleStatusOK || err != nil {
			body = []byte(`{"status":"error"}`)
		}
		if encoder.Encode(json.RawMessage(body)) != nil {
			os.Exit(1)
		}
	}
	if scanner.Err() != nil {
		os.Exit(1)
	}
}
