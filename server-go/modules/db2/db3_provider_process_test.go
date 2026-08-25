//go:build linux

package db2

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	protocol "github.com/JBailes/aimee/server-go/db3"
	"golang.org/x/sys/unix"
)

// The shipped provider binary, as a separate process, on a real bus.
//
// WHAT THIS COVERS THAT NOTHING ELSE DID. Every other test of the
// externalization split runs the provider IN-PROCESS: routed_test.go builds a
// vectordb.Provider and calls it directly, and the C-host e2e attaches a
// goroutine that answers with a fake search function. In-process, no grant is
// ever checked, no executable is ever verified, and no deployment is ever
// exercised -- which is exactly how the provider came to have no runnable
// process at all while every test of it passed.
//
// Here the provider is the multicall binary under its aimee-module-db3-<name>
// name, started with the environment an operator sets, attaching under a grant
// that names its executable path. The bus checks /proc/<pid>/exe against that
// path, so a binary the grant does not name cannot attach at all.
//
// And it closes the loop rather than just proving attachment: points are
// APPLIED through the wire into the provider's index, then SEARCHED back
// through DB2's real router. A candidate returned here travelled DB2 -> bus ->
// provider process -> index -> back, with nothing faked on either side.

const providerProcessInstance = "probe"

// buildProviderBinary compiles the multicall and links it under the name the
// multicall dispatches on. The name IS the selector, so a test that ran the
// bare binary would exercise a different path than a deployment.
func buildProviderBinary(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	multicall := filepath.Join(dir, "aimee-module")
	build := exec.Command("go", "build", "-o", multicall, "./cmd/aimee-module")
	// The test runs in modules/db2, so the module root is two levels up.
	build.Dir = "../.."
	build.Env = append(os.Environ(), "CGO_ENABLED=0")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("building the multicall: %v\n%s", err, out)
	}
	// A hard link rather than a symlink: the grant compares the peer's
	// /proc/<pid>/exe, which resolves a symlink back to the multicall and would
	// then not match the name the grant was written for.
	provider := filepath.Join(dir, "aimee-module-db3-"+providerProcessInstance)
	if err := os.Link(multicall, provider); err != nil {
		t.Fatalf("linking the provider name: %v", err)
	}
	return provider
}

// TestTheShippedProviderBinaryServesOverARealBus runs the round trip against
// each backend the provider can be deployed with.
//
// The memory backend always runs. The qdrant one runs when a live Qdrant is
// named, and it is the case that matters most: it is the only test in the tree
// where a search leaves the process, crosses the bus, is answered out of a real
// vector database, and comes back. Everything else about the split is one half
// talking to a fake of the other.
func TestTheShippedProviderBinaryServesOverARealBus(t *testing.T) {
	backends := []struct {
		name string
		env  []string
	}{{name: "memory", env: []string{"AIMEE_DB3_BACKEND=memory"}}}
	if url := os.Getenv("AIMEE_TEST_QDRANT_URL"); url != "" {
		backends = append(backends, struct {
			name string
			env  []string
		}{name: "qdrant", env: []string{
			"AIMEE_DB3_BACKEND=qdrant",
			"AIMEE_DB3_QDRANT_URL=" + url,
			// A per-run prefix so a rerun never inherits the previous run's
			// points and two runs can share one Qdrant.
			fmt.Sprintf("AIMEE_DB3_QDRANT_PREFIX=busrt%d", time.Now().UnixNano()),
		}})
	}
	for _, backend := range backends {
		t.Run(backend.name, func(t *testing.T) {
			providerProcessRoundTrip(t, backend.env)
		})
	}
}

func providerProcessRoundTrip(t *testing.T, backendEnv []string) {
	harness := os.Getenv("DB3_GO_HOST")
	if harness == "" {
		t.Skip("DB3_GO_HOST is unset; run scripts/test_db3_go_bus.sh")
	}
	provider := buildProviderBinary(t)

	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	socket := filepath.Join(directory, "db3.sock")

	// The host grants provider A the provider binary's path, so this is the
	// deployment's authorization check and not a test's convenience.
	host := exec.Command(harness, socket, executable, provider)
	// Into a file rather than a pipe nothing reads: an unread pipe blocks the
	// writer once the buffer fills, so a host that ever logged would hang here
	// instead of failing.
	hostLog, err := os.Create(filepath.Join(directory, "host.log"))
	if err != nil {
		t.Fatal(err)
	}
	host.Stdout, host.Stderr = hostLog, hostLog
	if err := host.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = host.Process.Signal(unix.SIGTERM)
		_ = host.Wait()
		if t.Failed() {
			if body, readErr := os.ReadFile(filepath.Join(directory, "host.log")); readErr == nil && len(body) > 0 {
				t.Logf("DB3 C host output:\n%s", body)
			}
		}
	}()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(socket); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("DB3 host socket did not appear")
		}
		time.Sleep(5 * time.Millisecond)
	}

	// The dimension matches the request vector below. A provider at the wrong
	// width rejects every vector and answers nothing, which is why it is a
	// required setting rather than a defaulted one.
	command := exec.Command(provider, socket)
	command.Env = append(append(os.Environ(),
		"AIMEE_MODULE_PRINCIPAL_REF=456",
		"AIMEE_DB3_COLLECTION=memory",
		"AIMEE_DB3_DIMENSION=3",
		"AIMEE_DB3_METRIC=cosine",
	), backendEnv...)
	providerLog, err := os.Create(filepath.Join(directory, "provider.log"))
	if err != nil {
		t.Fatal(err)
	}
	command.Stdout, command.Stderr = providerLog, providerLog
	if err := command.Start(); err != nil {
		t.Fatalf("starting the provider process: %v", err)
	}
	defer func() {
		_ = command.Process.Signal(unix.SIGTERM)
		_ = command.Wait()
		if t.Failed() {
			if body, readErr := os.ReadFile(filepath.Join(directory, "provider.log")); readErr == nil {
				t.Logf("provider process output:\n%s", body)
			}
		}
	}()

	// Attaching as postgres (28), the only principal granted the caller role.
	db2Client := attachDB3Client(t, socket, 28)
	defer db2Client.Detach()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	router, busRouter, err := NewDB3BusRouter(ctx, db2Client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			// If the external path is working, DB2 never falls back to this.
			return protocol.SearchReply{RequestID: request.RequestID}, nil
		},
		func(_ context.Context, _, _ string, _ int64) (bool, error) { return true, nil },
		nil)
	if err != nil {
		t.Fatal(err)
	}

	// The provider process must publish capabilities and be routed to. This is
	// the step that fails if the grant, the band, the name, or the environment
	// is wrong -- each of which was wrong at some point in this work.
	deadline = time.Now().Add(10 * time.Second)
	for {
		outcome := router.Search(ctx, protocol.SearchRequest{
			RequestID: 1, RequiredGeneration: 1, Workspace: "workspace-a",
			Project: "project-a", RecordType: "memory", TopK: 3,
			Vector: []float32{1, 0, 0},
		})
		if outcome.Route == DB3External {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("the provider process never became the route (last outcome %+v)", outcome)
		}
		time.Sleep(20 * time.Millisecond)
	}

	// Apply real points through the wire, then search them back. The labels are
	// the ones scopeFilters derives from the request's scope, so a point that
	// did not carry them would be filtered out and the search would return
	// nothing -- which is the failure a fake provider can never show.
	labels := []protocol.ExactLabel{
		{Key: "project", Value: "project-a"},
		{Key: "record_type", Value: "memory"},
		{Key: "workspace", Value: "workspace-a"},
	}
	points := []struct {
		id     int64
		vector []float32
	}{
		{41, []float32{1, 0, 0}},
		{42, []float32{0.9, 0.1, 0}},
		{43, []float32{0, 0, 1}},
	}
	for i, point := range points {
		if err := busRouter.PublishApply(ctx, protocol.Apply{
			OperationID: uint64(i + 1), Generation: 1, PointID: point.id,
			Kind: protocol.ApplyUpsert, Collection: "memory",
			Vector: point.vector, Labels: labels,
		}); err != nil {
			t.Fatalf("publishing apply for point %d: %v", point.id, err)
		}
	}

	// DB2 searches AT a generation, and the wire requires the reply to carry
	// exactly that generation back. Every apply moves the provider forward, so
	// the generation to ask for is the one the router has been told the provider
	// reached -- which is the whole reason the provider republishes its
	// capabilities. Reading it here rather than assuming it is what makes this
	// test notice if that republication ever stops.
	// The applies are asynchronous, and against a remote store each one is a
	// network round trip, so the provider's generation is still moving while
	// this runs. The generation is therefore re-read on EVERY attempt rather
	// than once: a single read races the applies and then searches at a version
	// the provider has already left, which the router answers as unavailable.
	// Re-reading is also what DB2 does -- it searches at the generation it has
	// most recently been told about.
	deadline = time.Now().Add(30 * time.Second)
	var outcome DB3SearchOutcome
	var generation uint64
	for {
		route := router.Route(protocol.RouteRequest{RequestID: 3, Action: protocol.RouteQuery})
		if route.SelectedPrincipal == 456 && route.ProviderGeneration > 1 {
			generation = route.ProviderGeneration
			outcome = router.Search(ctx, protocol.SearchRequest{
				RequestID: 2, RequiredGeneration: generation, Workspace: "workspace-a",
				Project: "project-a", RecordType: "memory", TopK: 2,
				Vector: []float32{1, 0, 0},
			})
			if outcome.Route == DB3External && len(outcome.Reply.Candidates) == 2 {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatalf("the provider never returned the applied points (last generation %d, outcome %+v)",
				generation, outcome)
		}
		time.Sleep(50 * time.Millisecond)
	}

	// Nearest first, and the orthogonal point excluded by TopK. Getting the
	// ORDER right is what proves the provider scored rather than echoed.
	if outcome.Reply.Candidates[0].PointID != 41 || outcome.Reply.Candidates[1].PointID != 42 {
		t.Fatalf("candidates = %+v, want point 41 then 42", outcome.Reply.Candidates)
	}
	if outcome.Result != DB3OK {
		t.Fatalf("result = %v, want DB3OK", outcome.Result)
	}
}
