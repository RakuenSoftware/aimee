package delegates

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

// The deployment integration job opts in with an already-pulled test image.
// REQUIRE_DOCKER prevents a broken CI fixture from being reported as a skip.
func TestPrivateVerifierRealDocker(t *testing.T) {
	socket := os.Getenv("AIMEE_PRIVATE_TEST_DOCKER_SOCKET")
	required := os.Getenv("AIMEE_PRIVATE_TEST_REQUIRE_DOCKER") == "1"
	if socket == "" {
		if required {
			socket = "/var/run/docker.sock"
		} else {
			t.Skip("real Docker fixture not configured")
		}
	}
	image := os.Getenv("AIMEE_PRIVATE_TEST_DOCKER_IMAGE")
	if image == "" {
		t.Fatal("AIMEE_PRIVATE_TEST_DOCKER_IMAGE must name the pre-pulled configured test image")
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	engine, err := egress.OpenDockerEngine(ctx, socket)
	if err != nil {
		t.Fatal("configured Docker engine unavailable", err)
	}
	defer engine.Close()

	// Real daemon recovery: the resource owner survives helper death, whereas
	// a simultaneously live helper's independent lock prevents its removal.
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
	liveID, err := engine.CreateContainer(ctx, map[string]any{"Image": image, "Labels": live.Labels(), "Cmd": []string{"sleep", "infinity"}, "HostConfig": map[string]any{"NetworkMode": "none"}})
	if err != nil {
		orphan.Close(true)
		t.Fatal(err)
	}
	defer engine.Remove(context.Background(), liveID)
	orphanID, err := engine.CreateContainer(ctx, map[string]any{"Image": image, "Labels": orphan.Labels(), "Cmd": []string{"sleep", "infinity"}, "HostConfig": map[string]any{"NetworkMode": "none"}})
	if err != nil {
		orphan.Close(true)
		t.Fatal(err)
	}
	defer engine.Remove(context.Background(), orphanID)
	orphan.Close(false)
	if count, err := ReapPrivateWorkers(ctx, state, engine); err != nil || count != 1 {
		t.Fatalf("real orphan recovery count=%d err=%v", count, err)
	}
	if _, err := engine.InspectContainer(ctx, liveID); err != nil {
		t.Fatal("live verifier removed by orphan reaper", err)
	}
	if _, err := engine.InspectContainer(ctx, orphanID); err == nil {
		t.Fatal("orphan remains after removal acknowledgment")
	}
	root := t.TempDir()
	for _, args := range [][]string{{"init", "-q"}, {"config", "user.name", "Verifier Fixture"}, {"config", "user.email", "verifier@example.invalid"}} {
		if output, err := exec.Command("git", append([]string{"-C", root}, args...)...).CombinedOutput(); err != nil {
			t.Fatal(err, string(output))
		}
	}
	if err := os.WriteFile(filepath.Join(root, "source.txt"), []byte("committed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{{"add", "source.txt"}, {"commit", "-qm", "fixture"}} {
		if output, err := exec.Command("git", append([]string{"-C", root}, args...)...).CombinedOutput(); err != nil {
			t.Fatal(err, string(output))
		}
	}
	request := verifierRequest()
	if err := os.WriteFile(filepath.Join(root, ".git", "aimee-verification-lease"), []byte(request.SnapshotID+"\n"), 0400); err != nil {
		t.Fatal(err)
	}
	request.StepTimeoutMS = 10000
	request.EnvChecks = []string{"bash", "cat"}
	request.Steps = []PrivateTestStep{{Name: "read-private-source", Run: `test "$(cat source.txt)" = committed && test -d .git && ! test -S /run/aimee/aimee-http.sock && printf '\000\377\001'`}}
	facts := PrivateTestFacts{Image: image, WorkDir: root, User: fmt.Sprintf("%d:%d", os.Getuid(), os.Getgid()), MountTable: os.Getenv("AIMEE_PRIVATE_TEST_MOUNT_TABLE")}
	receipt := ExecutePrivateTests(ctx, request, facts, engine)
	if !receipt.Complete || !receipt.Passed || !receipt.Quiescent || !receipt.RemovalAcknowledged || len(receipt.Steps) != 1 || receipt.Steps[0].Observation == nil {
		t.Fatalf("real private worker did not qualify: %+v", receipt)
	}
	if !strings.HasPrefix(receipt.Profile.ImageID, "sha256:") || receipt.Profile.Digest == "" || string(receipt.Steps[0].Observation.Stdout) != string([]byte{0, 255, 1}) {
		t.Fatal("actual image identity or binary output lost", receipt)
	}
	// A daemon namespace can resolve the declared bind path to a different
	// directory while all inspect path strings still match. Only the random
	// marker demonstrates that the worker received this private snapshot.
	foreignRoot := t.TempDir()
	if err := os.Mkdir(filepath.Join(foreignRoot, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(foreignRoot, ".git", "aimee-verification-lease"), []byte(strings.Repeat("e", 32)+"\n"), 0400); err != nil {
		t.Fatal(err)
	}
	foreignFacts := facts
	foreignFacts.MountTable = root + "\t" + TranslateMountPath(foreignRoot, facts.MountTable) + "\n"
	wrongMount := ExecutePrivateTests(ctx, request, foreignFacts, engine)
	if wrongMount.Complete || wrongMount.Passed || wrongMount.MountIdentityVerified || wrongMount.ConfiguredStepsStarted != 0 || wrongMount.OrdinaryFallbackEligible || !wrongMount.Quiescent || !wrongMount.RemovalAcknowledged {
		t.Fatalf("wrong daemon filesystem checkout became verified source: %+v", wrongMount)
	}
	request.EnvChecks = []string{"aimee_fixture_tool_that_does_not_exist"}
	request.Steps = []PrivateTestStep{{Name: "must-not-run", Run: `printf unsafe > source.txt`}}
	missing := ExecutePrivateTests(ctx, request, facts, engine)
	if missing.Complete || missing.Passed || !missing.Quiescent || !missing.RemovalAcknowledged || len(missing.EnvChecks) != 1 || missing.EnvChecks[0].Status != "unavailable" || missing.Steps[0].Observation != nil || !missing.OrdinaryFallbackEligible || missing.ConfiguredStepsStarted != 0 {
		t.Fatalf("missing real prerequisite became a work-product grade: %+v", missing)
	}
	preserved, err := os.ReadFile(filepath.Join(root, "source.txt"))
	if err != nil || string(preserved) != "committed\n" {
		t.Fatal("test ran despite missing prerequisite", err)
	}
	request.EnvChecks = nil
	// A real timed-out process is joined by container removal before return.
	request.StepTimeoutMS = 40
	request.Steps = []PrivateTestStep{{Name: "cancelled", Run: `sleep 10; printf late > source.txt`}}
	started := time.Now()
	cancelled := ExecutePrivateTests(ctx, request, facts, engine)
	if cancelled.Complete || cancelled.Passed || !cancelled.Quiescent || !cancelled.RemovalAcknowledged || cancelled.OrdinaryFallbackEligible || cancelled.ConfiguredStepsStarted != 1 || time.Since(started) > 8*time.Second {
		t.Fatalf("real timed-out execution not joined: %+v", cancelled)
	}
	source, err := os.ReadFile(filepath.Join(root, "source.txt"))
	if err != nil || string(source) != "committed\n" {
		t.Fatal("late process changed source after cancellation", err)
	}
}
