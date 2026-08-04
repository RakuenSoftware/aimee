package main

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	testKind  uint32 = 5889
	testStage uint32 = 1
)

func handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if bytes.Equal(request, []byte("cancel")) {
		for !invocation.Cancelled() {
			time.Sleep(time.Millisecond)
		}
	}
	return append([]byte(nil), request...), bus.ModuleStatusOK
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s DAEMON_MODULE_BUS_SOCKET\n", os.Args[0])
		os.Exit(2)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	config := bus.ModuleProcessConfig{
		SocketPath:     os.Args[1],
		ModuleName:     "go-conformance-module",
		PrincipalClass: 1,
		PrincipalRef:   7,
		Stages:         []bus.ModuleStage{{EventKind: testKind, StageID: testStage}},
		Handler:        handle,
	}
	if err := bus.RunModuleProcess(ctx, config); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
