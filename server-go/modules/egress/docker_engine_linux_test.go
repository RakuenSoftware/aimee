package egress

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func dockerSocketFixture(t *testing.T, handler http.HandlerFunc) string {
	t.Helper()
	socket := filepath.Join(t.TempDir(), "engine.sock")
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: socket, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	listener.SetUnlinkOnClose(false)
	server := &http.Server{Handler: handler}
	go server.Serve(listener)
	t.Cleanup(func() { server.Close(); listener.Close() })
	return socket
}

func dockerTestFrame(stream byte, content []byte) []byte {
	frame := make([]byte, 8+len(content))
	frame[0] = stream
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(content)))
	copy(frame[8:], content)
	return frame
}

func TestDockerEngineObservedExecAndExactCleanup(t *testing.T) {
	container, execution := strings.Repeat("a", 64), strings.Repeat("b", 64)
	var removed atomic.Bool
	socket := dockerSocketFixture(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/version":
			json.NewEncoder(w).Encode(map[string]string{"ApiVersion": "1.56", "MinAPIVersion": "1.12"})
		case "/v1.56/containers/" + container + "/exec":
			var args map[string]any
			if r.Method != "POST" || json.NewDecoder(r.Body).Decode(&args) != nil || args["WorkingDir"] != "/workspace" || args["Tty"] != false {
				t.Error("lost exact owned execution arguments")
			}
			w.WriteHeader(http.StatusCreated)
			json.NewEncoder(w).Encode(map[string]string{"Id": execution})
		case "/v1.56/exec/" + execution + "/start":
			w.Write(dockerTestFrame(1, []byte("stdout\x00opaque")))
			w.Write(dockerTestFrame(2, []byte("stderr")))
		case "/v1.56/exec/" + execution + "/json":
			json.NewEncoder(w).Encode(map[string]any{"ID": execution, "ContainerID": container, "Running": false, "ExitCode": 1})
		case "/v1.56/containers/" + container:
			if r.Method != "DELETE" || r.URL.RawQuery != "force=1&v=1" {
				t.Error("cleanup did not name the exact immutable worker")
			}
			removed.Store(true)
			w.WriteHeader(http.StatusNoContent)
		default:
			t.Error("unexpected Docker control request", r.URL.Path)
			w.WriteHeader(http.StatusNotFound)
		}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	engine, err := OpenDockerEngine(ctx, socket)
	if err != nil {
		t.Fatal(err)
	}
	defer engine.Close()
	result, err := engine.Exec(ctx, container, "/workspace", "configured test")
	if err != nil || !result.Complete || result.ExitCode != "1" || result.Termination != "exited" ||
		!bytes.Equal(result.Stdout, []byte("stdout\x00opaque")) || string(result.Stderr) != "stderr" || result.StdoutOmitted != "0" {
		t.Fatal("lost authoritative completion or byte fidelity", result, err)
	}
	if err = engine.Remove(ctx, container); err != nil || !removed.Load() {
		t.Fatal("worker cleanup was not acknowledged", err)
	}
}

func TestDockerEngineRefusesUnknownOrForeignCompletion(t *testing.T) {
	container, execution := strings.Repeat("a", 64), strings.Repeat("b", 64)
	for _, metadata := range []map[string]any{
		{"ID": execution, "ContainerID": container, "Running": false},
		{"ID": execution, "ContainerID": container, "Running": true, "ExitCode": 0},
		{"ID": execution, "ContainerID": strings.Repeat("c", 64), "Running": false, "ExitCode": 0},
		{"ID": strings.Repeat("c", 64), "ContainerID": container, "Running": false, "ExitCode": 0},
		{"ID": execution, "ContainerID": container, "Running": false, "ExitCode": 137},
	} {
		socket := dockerSocketFixture(t, func(w http.ResponseWriter, r *http.Request) {
			switch {
			case r.URL.Path == "/version":
				w.Write([]byte(`{"ApiVersion":"1.56","MinAPIVersion":"1.44"}`))
			case strings.HasSuffix(r.URL.Path, "/exec"):
				w.WriteHeader(http.StatusCreated)
				json.NewEncoder(w).Encode(map[string]string{"Id": execution})
			case strings.HasSuffix(r.URL.Path, "/start"):
				w.Write(dockerTestFrame(1, []byte("ALL TESTS PASSED")))
			case strings.HasSuffix(r.URL.Path, "/json"):
				json.NewEncoder(w).Encode(metadata)
			}
		})
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		engine, err := OpenDockerEngine(ctx, socket)
		if err != nil {
			t.Fatal(err)
		}
		result, err := engine.Exec(ctx, container, "/workspace", "configured test")
		engine.Close()
		cancel()
		if err == nil || result.Complete {
			t.Fatal("output text replaced independent completion evidence", metadata)
		}
	}
}

func TestDockerEnginePinsSocketAndRefusesAliasesAndRedirects(t *testing.T) {
	var requests atomic.Int32
	socket := dockerSocketFixture(t, func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		w.Write([]byte(`{"ApiVersion":"1.56","MinAPIVersion":"1.44"}`))
	})
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	engine, err := OpenDockerEngine(ctx, socket)
	if err != nil {
		t.Fatal(err)
	}
	defer engine.Close()
	moved := socket + ".held"
	if err = os.Rename(socket, moved); err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(socket, []byte("replacement"), 0600); err != nil {
		t.Fatal(err)
	}
	if err = engine.negotiate(ctx); err != nil || requests.Load() != 2 {
		t.Fatal("pathname replacement displaced the held control resource", err)
	}
	if _, err = engine.Exec(ctx, "mutable-container-name", "/workspace", "true"); err == nil || requests.Load() != 2 {
		t.Fatal("a mutable name selected execution authority")
	}
	if _, err = OpenDockerEngine(ctx, socket); err == nil {
		t.Fatal("ordinary file became a Docker control socket")
	}
	redirect := dockerSocketFixture(t, func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "http://127.0.0.1:1/", http.StatusFound)
	})
	if _, err = OpenDockerEngine(ctx, redirect); err == nil {
		t.Fatal("control resource redirected outside Unix custody")
	}
}

func TestDockerOutputFrameBoundsAndCompleteByteTails(t *testing.T) {
	stdout, stderr := &dockerTail{maximum: 4}, &dockerTail{maximum: 4}
	if err := readDockerOutput(bytes.NewReader(dockerTestFrame(1, []byte("abcdefgh"))), stdout, stderr); err != nil || string(stdout.data) != "efgh" || stdout.total != 8 {
		t.Fatal("output tail lost its byte accounting", err)
	}
	for _, raw := range [][]byte{dockerTestFrame(0, []byte("unknown stream")), {1, 0, 0}, dockerTestFrame(1, []byte("short"))[:10]} {
		if err := readDockerOutput(bytes.NewReader(raw), stdout, stderr); err == nil {
			t.Fatal("invalid or incomplete output became completion evidence")
		}
	}
}
