// aimee-economizer-probe exposes the production economizer handler as a
// newline-delimited stdin/stdout process for reproducible benchmark runs.
//
// It deliberately has no provider or network access. Each input line is the
// exact economizer.ReduceRequest wire object; each output line is the exact
// economizer.ReduceResponse wire object returned by the production handler.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/economizer"
)

// memoryStateStore gives multi-turn benchmark conversations the same
// StateStore contract that production serves from DB1. The probe remains
// process-local and disposable, but a state_key now exercises warm freeze
// boundaries and recall state instead of silently degrading every turn cold.
type memoryStateStore struct {
	mu     sync.Mutex
	states map[string]string
}

func (s *memoryStateStore) LoadState(_ context.Context, key string) (string, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	value, found := s.states[key]
	return value, found, nil
}

func (s *memoryStateStore) SaveState(_ context.Context, key, value string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.states[key] = value
	return nil
}

func main() {
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 64*1024), 32*1024*1024)
	handler := economizer.NewHandlerWithStore(&memoryStateStore{
		states: make(map[string]string),
	})
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
