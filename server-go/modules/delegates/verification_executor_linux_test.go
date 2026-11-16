package delegates

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

func TestPrivateVerifierThroughHeldUnixEngineLifecycle(t *testing.T) {
	for _, mode := range []string{"success", "lost removal", "cancelled execution", "wrong mounted checkout"} {
		t.Run(mode, func(t *testing.T) {
			directory, err := os.MkdirTemp("", "vx-")
			if err != nil {
				t.Fatal(err)
			}
			defer os.RemoveAll(directory)
			socket := filepath.Join(directory, "engine.sock")
			listener, err := net.Listen("unix", socket)
			if err != nil {
				t.Fatal(err)
			}
			container, execution := strings.Repeat("a", 64), strings.Repeat("b", 64)
			var mutex sync.Mutex
			var configuration map[string]any
			var activeCommand string
			var lifecycle []string
			handler := func(w http.ResponseWriter, r *http.Request) {
				mutex.Lock()
				lifecycle = append(lifecycle, r.Method+" "+r.URL.Path)
				mutex.Unlock()
				switch {
				case r.URL.Path == "/version":
					json.NewEncoder(w).Encode(map[string]string{"ApiVersion": "1.56", "MinAPIVersion": "1.44"})
				case r.URL.Path == "/v1.56/containers/create":
					if err := json.NewDecoder(r.Body).Decode(&configuration); err != nil {
						t.Error(err)
					}
					w.WriteHeader(201)
					json.NewEncoder(w).Encode(map[string]string{"Id": container})
				case r.URL.Path == "/v1.56/containers/"+container+"/start":
					w.WriteHeader(204)
				case r.URL.Path == "/v1.56/containers/"+container+"/json":
					host := configuration["HostConfig"].(map[string]any)
					binds := host["Binds"].([]any)
					if len(binds) != 1 || host["NetworkMode"] != "none" {
						t.Error("widened private worker resources")
					}
					source, target, _ := strings.Cut(binds[0].(string), ":")
					json.NewEncoder(w).Encode(map[string]any{"Id": container, "Image": "sha256:" + strings.Repeat("d", 64),
						"State": map[string]any{"Running": true}, "Config": map[string]any{"User": configuration["User"], "WorkingDir": configuration["WorkingDir"], "Env": configuration["Env"], "Cmd": configuration["Cmd"], "Entrypoint": []string{}, "Healthcheck": configuration["Healthcheck"]},
						"HostConfig":      map[string]any{"NetworkMode": "none", "Privileged": false, "IpcMode": "private", "PidMode": ""},
						"NetworkSettings": map[string]any{"Networks": map[string]any{"none": map[string]string{"IPAddress": ""}}},
						"Mounts":          []map[string]any{{"Source": source, "Destination": target, "RW": true}}})
				case r.URL.Path == "/v1.56/containers/"+container+"/exec":
					var args struct{ Cmd []string }
					json.NewDecoder(r.Body).Decode(&args)
					if len(args.Cmd) != 3 || args.Cmd[0] != "bash" || args.Cmd[1] != "-c" || (args.Cmd[2] != "configured exact test" && args.Cmd[2] != "cat -- .git/aimee-verification-lease") {
						t.Error("untrusted command substitution")
					}
					mutex.Lock()
					activeCommand = args.Cmd[2]
					mutex.Unlock()
					w.WriteHeader(201)
					json.NewEncoder(w).Encode(map[string]string{"Id": execution})
				case r.URL.Path == "/v1.56/exec/"+execution+"/start":
					mutex.Lock()
					command := activeCommand
					mutex.Unlock()
					if command == "cat -- .git/aimee-verification-lease" {
						marker := []byte(strings.Repeat("f", 32) + "\n")
						if mode == "wrong mounted checkout" {
							marker = []byte(strings.Repeat("e", 32) + "\n")
						}
						frame := make([]byte, 8+len(marker))
						frame[0] = 1
						binary.BigEndian.PutUint32(frame[4:8], uint32(len(marker)))
						copy(frame[8:], marker)
						w.Write(frame)
						return
					}

					if mode == "cancelled execution" {
						<-r.Context().Done()
						return
					}
					frame := make([]byte, 11)
					frame[0] = 1
					binary.BigEndian.PutUint32(frame[4:8], 3)
					copy(frame[8:], []byte{0, 255, 1})
					w.Write(frame)
				case r.URL.Path == "/v1.56/exec/"+execution+"/json":
					json.NewEncoder(w).Encode(map[string]any{"ID": execution, "ContainerID": container, "Running": false, "ExitCode": 0})
				case r.URL.Path == "/v1.56/containers/"+container && r.Method == http.MethodDelete:
					if r.URL.RawQuery != "force=1&v=1" {
						t.Error("cleanup not exact")
					}
					if mode == "lost removal" {
						w.WriteHeader(500)
					} else {
						w.WriteHeader(204)
					}
				default:
					t.Error("unexpected lifecycle request", r.Method, r.URL.Path)
					w.WriteHeader(404)
				}
			}
			server := &http.Server{Handler: http.HandlerFunc(handler)}
			go server.Serve(listener)
			defer server.Close()
			defer listener.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel()
			engine, err := egress.OpenDockerEngine(ctx, socket)
			if err != nil {
				t.Fatal(err)
			}
			defer engine.Close()
			request := verifierRequest()
			request.Steps = []PrivateTestStep{{Name: "test", Run: "configured exact test"}}
			if mode == "cancelled execution" {
				request.StepTimeoutMS = 20
			}
			receipt := ExecutePrivateTests(ctx, request, verifierFacts(t), engine)
			mutex.Lock()
			defer mutex.Unlock()
			if len(lifecycle) < 6 || lifecycle[len(lifecycle)-1] != "DELETE /v1.56/containers/"+container {
				t.Fatal("worker not joined before receipt", lifecycle)
			}
			if mode == "success" {
				if !receipt.Complete || !receipt.Passed || !receipt.Quiescent || len(receipt.Steps[0].Observation.Stdout) != 3 {
					t.Fatalf("real API lifecycle lost receipt: %+v", receipt)
				}
			} else if receipt.Complete || receipt.Passed {
				t.Fatalf("unqualified lifecycle passed: %+v", receipt)
			}
			if mode == "wrong mounted checkout" && (receipt.MountIdentityVerified || receipt.ConfiguredStepsStarted != 0 || receipt.OrdinaryFallbackEligible) {
				t.Fatal("wrong mount could run configured effects", receipt)
			}
			if (mode != "lost removal") != receipt.RemovalAcknowledged {
				t.Fatal("incorrect cleanup acknowledgement", receipt)
			}
		})
	}
}

func TestPrivateVerifierReaperRequiresOwnerLockAndExactLabels(t *testing.T) {
	for _, mode := range []string{"normal", "changed labels", "lost removal"} {
		t.Run(mode, func(t *testing.T) {
			state := filepath.Join(t.TempDir(), "owners")
			live, err := AcquirePrivateWorkerLease(state)
			if err != nil {
				t.Fatal(err)
			}
			defer live.Close(true)
			orphan, err := AcquirePrivateWorkerLease(state)
			if err != nil {
				t.Fatal(err)
			}
			if live.owner != orphan.owner || live.worker == orphan.worker {
				t.Fatal("worker ownership not stable and concurrent")
			}
			orphanLabels := orphan.Labels()
			orphan.Close(false)
			foreign := map[string]string{verifierKindLabel: verifierKind, verifierOwnerLabel: strings.Repeat("f", 64), verifierWorkerLabel: orphanLabels[verifierWorkerLabel]}
			ids := []string{strings.Repeat("a", 64), strings.Repeat("b", 64), strings.Repeat("c", 64)}
			labels := []map[string]string{live.Labels(), orphanLabels, foreign}
			directory, err := os.MkdirTemp("", "vr-")
			if err != nil {
				t.Fatal(err)
			}
			defer os.RemoveAll(directory)
			listener, err := net.Listen("unix", filepath.Join(directory, "docker.sock"))
			if err != nil {
				t.Fatal(err)
			}
			var mutex sync.Mutex
			deletes := 0
			inspects := 0
			server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch {
				case r.URL.Path == "/version":
					json.NewEncoder(w).Encode(map[string]string{"ApiVersion": "1.56"})
				case r.URL.Path == "/v1.56/containers/json":
					var filters map[string][]string
					if json.Unmarshal([]byte(r.URL.Query().Get("filters")), &filters) != nil || len(filters["label"]) != 2 {
						t.Error("unscoped list")
					}
					json.NewEncoder(w).Encode([]map[string]string{{"Id": ids[0]}, {"Id": ids[1]}, {"Id": ids[2]}})
				case r.Method == "DELETE":
					if r.URL.Path != "/v1.56/containers/"+ids[1] {
						t.Error("removed live or foreign container")
					}
					mutex.Lock()
					deletes++
					mutex.Unlock()
					if mode == "lost removal" {
						w.WriteHeader(500)
					} else {
						w.WriteHeader(204)
					}
				default:
					for i, id := range ids {
						if r.URL.Path == "/v1.56/containers/"+id+"/json" {
							actual := labels[i]
							if i == 1 {
								mutex.Lock()
								inspects++
								count := inspects
								mutex.Unlock()
								if mode == "changed labels" && count > 1 {
									actual = foreign
								}
							}
							json.NewEncoder(w).Encode(map[string]any{"Id": id, "Config": map[string]any{"Labels": actual}})
							return
						}
					}
					t.Error("unexpected reaper request " + r.URL.Path)
					w.WriteHeader(500)
				}
			})}
			go server.Serve(listener)
			defer server.Close()
			engine, err := egress.OpenDockerEngine(context.Background(), filepath.Join(directory, "docker.sock"))
			if err != nil {
				t.Fatal(err)
			}
			defer engine.Close()
			count, err := ReapPrivateWorkers(context.Background(), state, engine)
			if mode == "lost removal" {
				if err == nil || count != 0 {
					t.Fatal("lost removal acknowledged")
				}
			} else if err != nil {
				t.Fatal(err)
			}
			mutex.Lock()
			got := deletes
			mutex.Unlock()
			if mode == "changed labels" {
				if got != 0 || count != 0 {
					t.Fatal("changed ownership removed")
				}
			} else if got != 1 {
				t.Fatalf("expected one orphan removal, got %d", got)
			}
			if mode == "normal" && count != 1 {
				t.Fatal("orphan removal not acknowledged")
			}
			if mode != "normal" {
				if _, err := os.Stat(filepath.Join(state, orphanLabels[verifierWorkerLabel]+".lock")); err != nil {
					t.Fatal("unacknowledged worker custody lost")
				}
			}
		})
	}
}

func TestPrivateVerifierOwnerStateRejectsSymlinkAndPublicDirectory(t *testing.T) {
	root := t.TempDir()
	target := filepath.Join(root, "target")
	if err := os.Mkdir(target, 0700); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "link")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	if lease, err := AcquirePrivateWorkerLease(link); err == nil {
		lease.Close(true)
		t.Fatal("accepted symlink owner state")
	}
	if err := os.Chmod(target, 0755); err != nil {
		t.Fatal(err)
	}
	if lease, err := AcquirePrivateWorkerLease(target); err == nil {
		lease.Close(true)
		t.Fatal("accepted public owner state")
	}
}
