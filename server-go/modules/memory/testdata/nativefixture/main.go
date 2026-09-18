// Native compatibility tests call the real Go memory owner. This executable is
// test-only: its trusted host invocation is never exposed on a network endpoint.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"os"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
)

type benchmarkStore struct{}

func (benchmarkStore) Get(context.Context, memory.Scope, int64) (memory.Record, error) {
	return memory.Record{}, errors.New("fixture read unsupported")
}
func (benchmarkStore) Put(context.Context, memory.Scope, memory.Record) (memory.Record, error) {
	return memory.Record{}, errors.New("fixture write unsupported")
}
func (benchmarkStore) Delete(context.Context, memory.Scope, int64) (bool, error) {
	return false, errors.New("fixture write unsupported")
}
func (benchmarkStore) Search(_ context.Context, _ memory.Scope, query, _, _ string, _ int) ([]memory.Record, error) {
	if query == "failure" {
		return nil, errors.New("fixture retrieval failed")
	}
	if query == "empty" {
		return []memory.Record{}, nil
	}
	return []memory.Record{{ID: 1, Content: strings.Repeat("界", 2100)}, {ID: 2, Content: "second"}}, nil
}
func main() {
	var data memory.DataStore
	if os.Getenv("AIMEE_TEST_BENCHMARK_FIXTURE") == "1" {
		data = benchmarkStore{}
	}
	handler := memory.NewHandler(nil, memory.WithDataStore(memory.PlacementServer, data))
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
