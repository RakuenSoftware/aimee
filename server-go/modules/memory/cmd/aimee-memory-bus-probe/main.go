// aimee-memory-bus-probe sends one JSON memory-data request over a live daemon
// module bus. It is validation tooling: principal 200 is intentionally outside
// the shipped registry and must be admitted by an explicit test-only grant.
package main

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
)

const probePrincipalRef uint32 = 200

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: aimee-memory-bus-probe BUS_SOCKET JSON_REQUEST")
		os.Exit(2)
	}
	ctx := context.Background()
	client, err := bus.ConnectClient(ctx, os.Args[1], 1, probePrincipalRef)
	if err != nil {
		fmt.Fprintf(os.Stderr, "attach: %v\n", err)
		os.Exit(1)
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		fmt.Fprintf(os.Stderr, "caller: %v\n", err)
		os.Exit(1)
	}
	reply, err := caller.Call(ctx, memory.EventData, memory.StageData, 0, 15*time.Second,
		[]byte(os.Args[2]))
	caller.CloseAndWait()
	client.Detach()
	if err != nil {
		fmt.Fprintf(os.Stderr, "call: %v\n", err)
		os.Exit(1)
	}
	fmt.Println(string(reply))
}
