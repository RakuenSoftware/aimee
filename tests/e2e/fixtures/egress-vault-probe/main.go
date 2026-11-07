// Test-only MCP caller: the daemon must explicitly grant its pinned executable
// the MCP egress identity 712. It never receives or prints a bearer credential.
package main

import (
	"context"
	"fmt"
	"os"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/mcp"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

func probe() error {
	if len(os.Args) != 4 || (os.Args[3] != "exchange" && os.Args[3] != "foreign-handle") {
		return fmt.Errorf("invalid probe invocation")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	client, err := ring.ConnectClient(ctx, os.Args[1], 1, 200+egress.PluginClientOffset)
	if err != nil {
		return err
	}
	defer client.Detach()
	caller, err := ring.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		return err
	}
	defer caller.CloseAndWait()
	transport, err := egress.NewBusAuthorizer(caller)
	if err != nil {
		return err
	}
	handle := "mcp:712"
	if os.Args[3] == "foreign-handle" {
		handle = "mcp:713"
	}
	stream, err := mcp.NewSSETransport(os.Args[2], handle, 10*time.Second, transport, 1)
	if os.Args[3] == "foreign-handle" {
		if err == nil {
			stream.Close()
			return fmt.Errorf("foreign credential handle admitted")
		}
		fmt.Println("denied")
		return nil
	}
	if err != nil {
		return err
	}
	defer stream.Close()
	frame := []byte(`{"jsonrpc":"2.0","id":1,"method":"fixture/echo"}`)
	if err = stream.Send(frame); err != nil {
		return err
	}
	received, err := stream.Recv(10 * time.Second)
	if err != nil || string(received) != string(frame) {
		return fmt.Errorf("MCP exchange failed")
	}
	fmt.Println("ok")
	return nil
}

func main() {
	if probe() != nil {
		// Transport failures may contain provider-controlled bytes. Output a fixed
		// diagnostic so credentials cannot enter logs even on the failure path.
		fmt.Fprintln(os.Stderr, "egress Vault probe failed")
		os.Exit(1)
	}
}
