package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

func TestVerifierParentDeathRemovesWorker(t *testing.T) {
	switch os.Getenv("AIMEE_VERIFIER_LIFETIME_ROLE") {
	case "parent":
		input, err := os.Open(os.Getenv("AIMEE_VERIFIER_LIFETIME_REQUEST"))
		if err != nil {
			t.Fatal(err)
		}
		defer input.Close()
		output, err := os.Create(os.Getenv("AIMEE_VERIFIER_LIFETIME_RESULT"))
		if err != nil {
			t.Fatal(err)
		}
		defer output.Close()
		command := exec.Command(os.Args[0], "-test.run=^TestVerifierParentDeathRemovesWorker$", "-test.timeout=20s")
		command.Env = append(os.Environ(), "AIMEE_VERIFIER_LIFETIME_ROLE=worker", "AIMEE_VERIFIER_EXPECTED_PARENT="+strconv.Itoa(os.Getpid()))
		command.Stdin = input
		command.Stdout = output
		command.Stderr = os.Stderr
		if err = command.Start(); err != nil {
			t.Fatal(err)
		}
		if err = os.WriteFile(os.Getenv("AIMEE_VERIFIER_LIFETIME_PID"), []byte(strconv.Itoa(command.Process.Pid)), 0600); err != nil {
			t.Fatal(err)
		}
		if err = command.Wait(); err != nil {
			t.Fatal(err)
		}
		return
	case "worker":
		err := verify(context.Background(), []string{"--owner-state-dir", filepath.Join(os.Getenv("AIMEE_VERIFIER_LIFETIME_ROOT"), "owners"), "--parent-pid", os.Getenv("AIMEE_VERIFIER_EXPECTED_PARENT"), "--engine-socket", os.Getenv("AIMEE_VERIFIER_LIFETIME_SOCKET"), "--image", "fixture", "--workdir", os.Getenv("AIMEE_VERIFIER_LIFETIME_ROOT"), "--user", fmt.Sprintf("%d:%d", os.Getuid(), os.Getgid())})
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(2)
		}
		os.Exit(0)
	}
	directory, err := os.MkdirTemp("", "vl-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(directory)
	root := filepath.Join(directory, "snapshot")
	if err = os.MkdirAll(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, ".git", "aimee-verification-lease"), []byte(strings.Repeat("f", 32)+"\n"), 0400); err != nil {
		t.Fatal(err)
	}
	socket := filepath.Join(directory, "engine.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	container, execution := strings.Repeat("a", 64), strings.Repeat("b", 64)
	var configuration map[string]any
	var activeCommand string
	var lock sync.Mutex
	started, removed := make(chan struct{}), make(chan struct{})
	var startOnce, removeOnce sync.Once
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/version":
			w.Write([]byte(`{"ApiVersion":"1.56","MinAPIVersion":"1.44"}`))
		case "/v1.56/containers/create":
			var value map[string]any
			if err := json.NewDecoder(r.Body).Decode(&value); err != nil {
				t.Error(err)
			}
			lock.Lock()
			configuration = value
			lock.Unlock()
			w.WriteHeader(201)
			json.NewEncoder(w).Encode(map[string]string{"Id": container})
		case "/v1.56/containers/" + container + "/start":
			w.WriteHeader(204)
		case "/v1.56/containers/" + container + "/json":
			lock.Lock()
			value := configuration
			lock.Unlock()
			source, target, _ := strings.Cut(value["HostConfig"].(map[string]any)["Binds"].([]any)[0].(string), ":")
			json.NewEncoder(w).Encode(map[string]any{"Id": container, "Image": "sha256:" + strings.Repeat("c", 64), "State": map[string]any{"Running": true},
				"Config":          map[string]any{"User": value["User"], "WorkingDir": root, "Env": value["Env"], "Cmd": value["Cmd"], "Entrypoint": []string{}, "Healthcheck": value["Healthcheck"]},
				"HostConfig":      map[string]any{"NetworkMode": "none", "Privileged": false, "IpcMode": "private", "PidMode": ""},
				"NetworkSettings": map[string]any{"Networks": map[string]any{"none": map[string]string{"IPAddress": ""}}}, "Mounts": []map[string]any{{"Source": source, "Destination": target, "RW": true}}})
		case "/v1.56/containers/" + container + "/exec":
			var args struct{ Cmd []string }
			if err := json.NewDecoder(r.Body).Decode(&args); err != nil || len(args.Cmd) != 3 {
				t.Error("invalid owned exec")
				w.WriteHeader(400)
				return
			}
			lock.Lock()
			activeCommand = args.Cmd[2]
			lock.Unlock()
			w.WriteHeader(201)
			json.NewEncoder(w).Encode(map[string]string{"Id": execution})
		case "/v1.56/exec/" + execution + "/start":
			lock.Lock()
			command := activeCommand
			lock.Unlock()
			if command == "cat -- .git/aimee-verification-lease" {
				marker := []byte(strings.Repeat("f", 32) + "\n")
				frame := make([]byte, 8+len(marker))
				frame[0] = 1
				binary.BigEndian.PutUint32(frame[4:8], uint32(len(marker)))
				copy(frame[8:], marker)
				w.Write(frame)
				return
			}

			startOnce.Do(func() { close(started) })
			<-r.Context().Done()
		case "/v1.56/exec/" + execution + "/json":
			json.NewEncoder(w).Encode(map[string]any{"ID": execution, "ContainerID": container, "Running": false, "ExitCode": 0})
		case "/v1.56/containers/" + container:
			if r.Method != "DELETE" || r.URL.RawQuery != "force=1&v=1" {
				t.Error("cleanup was not exact")
			}
			w.WriteHeader(204)
			removeOnce.Do(func() { close(removed) })
		default:
			t.Error("unexpected Engine request", r.URL.Path)
			w.WriteHeader(404)
		}
	})}
	go server.Serve(listener)
	defer server.Close()
	defer listener.Close()
	request, result, pidfile := filepath.Join(directory, "request.json"), filepath.Join(directory, "result.json"), filepath.Join(directory, "worker.pid")
	encoded, _ := json.Marshal(delegates.PrivateTestRequest{Schema: "aimee.private-tests.request.v1", ConfigSHA256: strings.Repeat("d", 64), SourceRevision: "fixture", SnapshotID: strings.Repeat("f", 32), StepTimeoutMS: 30000, Steps: []delegates.PrivateTestStep{{Name: "test", Run: "sleep 30"}}})
	if err = os.WriteFile(request, encoded, 0600); err != nil {
		t.Fatal(err)
	}
	parent := exec.Command(os.Args[0], "-test.run=^TestVerifierParentDeathRemovesWorker$", "-test.timeout=20s")
	parent.Env = append(os.Environ(), "AIMEE_VERIFIER_LIFETIME_ROLE=parent", "AIMEE_VERIFIER_LIFETIME_REQUEST="+request, "AIMEE_VERIFIER_LIFETIME_RESULT="+result, "AIMEE_VERIFIER_LIFETIME_PID="+pidfile, "AIMEE_VERIFIER_LIFETIME_SOCKET="+socket, "AIMEE_VERIFIER_LIFETIME_ROOT="+root)
	parent.Stderr = os.Stderr
	if err = parent.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { parent.Process.Kill(); parent.Wait() }()
	defer func() {
		if raw, err := os.ReadFile(pidfile); err == nil {
			if pid, err := strconv.Atoi(string(raw)); err == nil {
				syscall.Kill(pid, syscall.SIGKILL)
			}
		}
	}()
	select {
	case <-started:
	case <-time.After(5 * time.Second):
		t.Fatal("worker did not reach live configured execution")
	}
	if err = parent.Process.Kill(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-removed:
	case <-time.After(3 * time.Second):
		t.Fatal("parent death did not cancel and remove private worker")
	}
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		var receipt delegates.PrivateTestReceipt
		if raw, err := os.ReadFile(result); err == nil && json.Unmarshal(raw, &receipt) == nil {
			if receipt.Complete || receipt.Passed || !receipt.Quiescent || !receipt.RemovalAcknowledged || receipt.ConfiguredStepsStarted != 1 || receipt.OrdinaryFallbackEligible {
				t.Fatalf("parent death became completed work or retry: %+v", receipt)
			}
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("joined parent-death receipt was not written")
}
