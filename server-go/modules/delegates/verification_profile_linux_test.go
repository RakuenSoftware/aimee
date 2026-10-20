package delegates

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

func privateProfileRequest(t *testing.T, root string) DockerCreateRequest {
	t.Helper()
	spec, err := BuildSandboxSpec(SandboxRequest{WritesAllowed: true, IsGitCheckout: true,
		RepoRoot: root, Worktree: root, ParentSocketHost: "/run/aimee/aimee-http.sock",
		ParentSocketTarget: ControlSocketTarget, RunAsUser: "1000:1000"})
	if err != nil {
		t.Fatal(err)
	}
	return DockerCreateRequest{Spec: spec, WorkDir: root, ContainerName: "private-test", Image: "configured-tag"}
}

func profileEngineFixture(t *testing.T, request DockerCreateRequest, mutate func(map[string]any)) *egress.DockerEngine {
	t.Helper()
	var mounts []map[string]any
	for _, mount := range request.Spec.Mounts {
		mounts = append(mounts, map[string]any{"Source": mount.Source, "Destination": mount.Target, "RW": !mount.ReadOnly})
	}
	environment := []string{"PATH=/usr/bin:/bin"}
	for _, entry := range request.Spec.Env {
		environment = append(environment, entry.Name+"="+entry.Value)
	}
	metadata := map[string]any{
		"Id": strings.Repeat("a", 64), "Image": "sha256:" + strings.Repeat("b", 64),
		"State":           map[string]any{"Running": true},
		"Config":          map[string]any{"User": request.Spec.User, "WorkingDir": request.WorkDir, "Env": environment},
		"HostConfig":      map[string]any{"NetworkMode": "none", "Privileged": false, "PidMode": "", "IpcMode": "private"},
		"NetworkSettings": map[string]any{"Networks": map[string]any{"none": map[string]string{"IPAddress": ""}}}, "Mounts": mounts,
	}
	if mutate != nil {
		mutate(metadata)
	}
	body, _ := json.Marshal(metadata)
	directory, err := os.MkdirTemp("", "vp-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(directory) })
	socket := filepath.Join(directory, "docker.sock")
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/version" {
			w.Write([]byte(`{"ApiVersion":"1.56","MinAPIVersion":"1.44"}`))
			return
		}
		w.Write(body)
	})}
	go server.Serve(listener)
	t.Cleanup(func() { server.Close(); listener.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	engine, err := egress.OpenDockerEngine(ctx, socket)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(engine.Close)
	return engine
}

func TestPrivateTestProfileUsesActualImageAndStablePolicyCohort(t *testing.T) {
	runner := ContainerRunner{}
	var digest string
	for _, root := range []string{"/private/snapshot-a", "/private/snapshot-b"} {
		request := privateProfileRequest(t, root)
		engine := profileEngineFixture(t, request, nil)
		profile, err := runner.AttestPrivateTestContainer(context.Background(), request, strings.Repeat("a", 64), engine)
		if err != nil || profile.ImageID != "sha256:"+strings.Repeat("b", 64) || len(profile.Digest) != 64 {
			t.Fatal("missing observed image/profile", profile, err)
		}
		if digest != "" && digest != profile.Digest {
			t.Fatal("instance path changed the policy cohort")
		}
		digest = profile.Digest
	}
}

func TestPrivateTestProfileRefusesUnobservedOrWiderResources(t *testing.T) {
	for name, mutate := range map[string]func(map[string]any){
		"configured image label": func(m map[string]any) { m["Image"] = "configured-tag" },
		"foreign container":      func(m map[string]any) { m["Id"] = strings.Repeat("c", 64) },
		"missing environment":    func(m map[string]any) { delete(m["Config"].(map[string]any), "Env") },
		"credential": func(m map[string]any) {
			config := m["Config"].(map[string]any)
			config["Env"] = append(config["Env"].([]string), "CLOUD_SECRET=forbidden")
		},
		"wrong directory": func(m map[string]any) { m["Config"].(map[string]any)["WorkingDir"] = "/other" },
		"privileged":      func(m map[string]any) { m["HostConfig"].(map[string]any)["Privileged"] = true },
		"host PID":        func(m map[string]any) { m["HostConfig"].(map[string]any)["PidMode"] = "host" },
		"network":         func(m map[string]any) { m["HostConfig"].(map[string]any)["NetworkMode"] = "bridge" },
		"wrong source": func(m map[string]any) {
			m["Mounts"].([]map[string]any)[0]["Source"] = "/other/source"
		},
	} {
		t.Run(name, func(t *testing.T) {
			request := privateProfileRequest(t, "/private/snapshot")
			engine := profileEngineFixture(t, request, mutate)
			if _, err := (ContainerRunner{}).AttestPrivateTestContainer(context.Background(), request, strings.Repeat("a", 64), engine); err == nil {
				t.Fatal("unqualified runtime became a private test profile")
			}
		})
	}
	request := startReq(t)
	request.WorkDir = writeReq().Worktree
	engine := profileEngineFixture(t, request, nil)
	if _, err := (ContainerRunner{}).AttestPrivateTestContainer(context.Background(), request, strings.Repeat("a", 64), engine); err == nil {
		t.Fatal("shared parent checkout/Git metadata became private snapshot custody")
	}
}
