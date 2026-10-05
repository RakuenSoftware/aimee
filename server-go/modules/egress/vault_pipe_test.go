package egress

import (
	"context"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestVaultPipeFixture(t *testing.T) {
	mode := os.Getenv("AIMEE_TEST_VAULT_PIPE")
	if mode == "" {
		return
	}
	if mode == "bad-ready" {
		os.Stdout.Write([]byte("NOPE"))
		os.Exit(0)
	}
	os.Stdout.Write([]byte("EVR1"))
	for {
		var size [4]byte
		if _, err := io.ReadFull(os.Stdin, size[:]); err != nil {
			os.Exit(0)
		}
		name := make([]byte, binary.LittleEndian.Uint32(size[:]))
		io.ReadFull(os.Stdin, name)
		if mode == "stall" {
			time.Sleep(time.Minute)
			os.Exit(1)
		}
		if mode == "large" {
			binary.LittleEndian.PutUint32(size[:], maxResolvedCredentialBytes+1)
			os.Stdout.Write(size[:])
			os.Exit(0)
		}
		if mode == "truncate" {
			binary.LittleEndian.PutUint32(size[:], 10)
			os.Stdout.Write(size[:])
			os.Stdout.Write([]byte("short"))
			os.Exit(0)
		}
		value := []byte("fixture-" + string(name))
		binary.LittleEndian.PutUint32(size[:], uint32(len(value)))
		os.Stdout.Write(size[:])
		os.Stdout.Write(value)
	}
}

func pipeFixture(t *testing.T, mode string) string {
	t.Helper()
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	helper := filepath.Join(t.TempDir(), "helper")
	// Executable paths come from the test runner, not operator input.
	if err := os.WriteFile(helper, []byte("#!/bin/sh\nexport AIMEE_TEST_VAULT_PIPE="+mode+"\nexec "+"'"+strings.ReplaceAll(executable, "'", "'\"'\"'")+"'"+" -test.run=^TestVaultPipeFixture$\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return helper
}

func TestVaultPipeSerializesConcurrentCredentialFrames(t *testing.T) {
	pipe, err := startVaultPipe(context.Background(), pipeFixture(t, "normal"), "")
	if err != nil {
		t.Fatal(err)
	}
	defer pipe.close()
	var group sync.WaitGroup
	for i := 0; i < 16; i++ {
		group.Add(1)
		go func(i int) {
			defer group.Done()
			name := "AIMEE_MCP_" + strconv.Itoa(712+i) + "_TOKEN"
			value, err := pipe.request(context.Background(), name)
			if err != nil || string(value) != "fixture-"+name {
				t.Errorf("mixed or failed credential response: %v", err)
			}
			clear(value)
		}(i)
	}
	group.Wait()
}

func TestVaultPipeRejectsBadReadinessAndBrokenResponses(t *testing.T) {
	for _, mode := range []string{"bad-ready", "large", "truncate", "stall"} {
		t.Run(mode, func(t *testing.T) {
			pipe, err := startVaultPipe(context.Background(), pipeFixture(t, mode), "")
			if mode == "bad-ready" {
				if err == nil {
					pipe.close()
					t.Fatal("unauthenticated readiness accepted")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			defer pipe.close()
			ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
			defer cancel()
			value, err := pipe.request(ctx, "AIMEE_MEMORY_BACKEND_TOKEN")
			clear(value)
			if err == nil {
				t.Fatal("broken credential response accepted")
			}
			select {
			case <-pipe.done:
			case <-time.After(3 * time.Second):
				t.Fatal("failed helper was not reaped")
			}
		})
	}
}

func TestPrepareHandlerRetainsCredentialFreeNativeStartup(t *testing.T) {
	t.Setenv("AIMEE_EGRESS_CREDENTIAL_HELPER", "")
	handler, closePipe, done, err := PrepareHandler(context.Background())
	if err != nil || handler == nil || done != nil {
		t.Fatal("credential-free native startup regressed")
	}
	closePipe()
}
