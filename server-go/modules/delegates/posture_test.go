package delegates

import (
	"strings"
	"testing"
)

func postureReport(spec SandboxSpec) RuntimePostureReport {
	var mounts strings.Builder
	for mount := range expectedMounts(spec, "") {
		mounts.WriteString(mount)
		mounts.WriteByte(';')
	}
	var environment strings.Builder
	for _, entry := range spec.Env {
		environment.WriteString(entry.Name + "=" + entry.Value + "\n")
	}
	environment.WriteString("PATH=/usr/bin:/bin\n")
	return RuntimePostureReport{Network: `{"none":{"IPAddress":""}}`, Mounts: mounts.String(), Environment: environment.String()}
}

func TestRuntimePostureFailsClosed(t *testing.T) {
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatal(err)
	}
	if verdict := VerifyRuntimePosture(spec, "", postureReport(spec)); verdict.Refuse {
		t.Fatalf("confirmed posture refused: %s", verdict.Reason)
	}
	for name, mutate := range map[string]func(*RuntimePostureReport){
		"network breach":           func(r *RuntimePostureReport) { r.Network = "bridge=172.18.0.2;" },
		"network unverifiable":     func(r *RuntimePostureReport) { r.NetworkFailed = true },
		"mount unverifiable":       func(r *RuntimePostureReport) { r.MountsFailed = true },
		"extra docker socket":      func(r *RuntimePostureReport) { r.Mounts += "/var/run/docker.sock|/var/run/docker.sock|true;" },
		"environment unverifiable": func(r *RuntimePostureReport) { r.EnvironmentFailed = true },
		"credential":               func(r *RuntimePostureReport) { r.Environment += "AWS_SECRET_ACCESS_KEY=forbidden\n" },
		"wrong endpoint": func(r *RuntimePostureReport) {
			r.Environment = strings.ReplaceAll(r.Environment, ControlEndpoint, "unix:/tmp/other.sock")
		},
	} {
		t.Run(name, func(t *testing.T) {
			report := postureReport(spec)
			mutate(&report)
			if verdict := VerifyRuntimePosture(spec, "", report); !verdict.Refuse {
				t.Fatal("breached or unverified posture was accepted")
			}
		})
	}
}
