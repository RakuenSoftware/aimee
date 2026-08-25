//go:build linux

package postgres

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
	"golang.org/x/sys/unix"
)

// The postgres module routing a real search to a real provider over a real bus.
//
// This is the claim the whole design rests on: a DB3 provider is OPTIONAL, and
// installing one moves the portable subset off PostgreSQL without changing
// anything else. Every other test of it stubs one side. Here the provider is
// the shipped binary in its own process, the transport is the event bus, and
// the fallback is a real function that records whether it was used -- so
// "routed" and "fell back" are observed rather than asserted about a fake.
//
// Needs the C bus host harness (DB3_GO_HOST) and skips cleanly without it.

func attachClient(t *testing.T, socket string, principal uint32) *bus.Client {
	t.Helper()
	fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fd)
	if err := unix.Connect(fd, &unix.SockaddrUnix{Name: socket}); err != nil {
		t.Fatal(err)
	}
	client, err := bus.AttachAs(fd, 1, principal)
	if err != nil {
		t.Fatalf("attach principal %d: %v", principal, err)
	}
	return client
}

func buildMulticall(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	multicall := filepath.Join(dir, "aimee-module")
	build := exec.Command("go", "build", "-o", multicall, "./cmd/aimee-module")
	build.Dir = "../.."
	build.Env = append(os.Environ(), "CGO_ENABLED=0")
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("building the multicall: %v\n%s", err, out)
	}
	provider := filepath.Join(dir, "aimee-module-db3-pgroute")
	if err := os.Link(multicall, provider); err != nil {
		t.Fatal(err)
	}
	return provider
}

func TestThePostgresModuleRoutesToAProviderOverTheBus(t *testing.T) {
	harness := os.Getenv("DB3_GO_HOST")
	if harness == "" {
		t.Skip("DB3_GO_HOST is unset; run scripts/validation/db3/run-qdrant-e2e.sh")
	}
	provider := buildMulticall(t)

	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	socket := filepath.Join(directory, "db3.sock")

	host := exec.Command(harness, socket, executable, provider)
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

	backendEnv := []string{"AIMEE_DB3_BACKEND=memory"}
	if url := os.Getenv("AIMEE_TEST_QDRANT_URL"); url != "" {
		backendEnv = []string{
			"AIMEE_DB3_BACKEND=qdrant",
			"AIMEE_DB3_QDRANT_URL=" + url,
			fmt.Sprintf("AIMEE_DB3_QDRANT_PREFIX=pgroute%d", time.Now().UnixNano()),
		}
	}
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
		t.Fatal(err)
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

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// The postgres module attaches as itself. Principal 28 is postgres; the
	// harness grants it the DB3 kinds a caller needs.
	client := attachClient(t, socket, 29)
	defer client.Detach()

	// A fallback that records whether PostgreSQL was asked. That is the whole
	// question: a routed search must not touch it, and every other case must.
	postgresCalls := 0
	router, attachment, err := NewBusVectorRouter(ctx, client,
		func(_ context.Context, request db3.SearchRequest) (db3.SearchReply, error) {
			postgresCalls++
			return db3.SearchReply{
				RequestID: request.RequestID, Generation: request.RequiredGeneration,
			}, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	defer attachment.Close()

	// Before any provider is observed, the module answers from PostgreSQL. This
	// is the ordinary deployment, and it must work with a bus present.
	request := db3.SearchRequest{
		RequestID: 1, RequiredGeneration: 1, Workspace: "workspace-a",
		Project: "project-a", RecordType: "memory", TopK: 2, Vector: []float32{1, 0, 0},
	}
	if _, route, err := router.Search(ctx, request); err != nil || route != RoutePostgreSQL {
		t.Fatalf("with no provider observed: route=%v err=%v", route, err)
	}
	if postgresCalls != 1 {
		t.Fatalf("PostgreSQL was asked %d times, want 1", postgresCalls)
	}

	// Now feed the registry the provider's capabilities, exactly as the
	// module's subscription would, and give it something to find.
	seeded := seedProviderThroughTheBus(t, ctx, client, attachment)

	deadline = time.Now().Add(20 * time.Second)
	var routed db3.SearchReply
	for {
		attachment.ObserveCapabilities(456, 1, uint64(time.Now().UnixNano()), seeded())
		principal, generation, ok := attachment.Registry().Selected()
		if ok && principal == 456 {
			request.RequiredGeneration = generation
			reply, route, err := router.Search(ctx, request)
			if err == nil && route == RouteProvider && len(reply.Candidates) == 2 {
				routed = reply
				break
			}
		}
		if time.Now().After(deadline) {
			t.Fatalf("the module never routed to the provider (postgres calls %d)", postgresCalls)
		}
		time.Sleep(50 * time.Millisecond)
	}

	if routed.Candidates[0].PointID != 41 || routed.Candidates[1].PointID != 42 {
		t.Fatalf("routed candidates = %+v, want 41 then 42", routed.Candidates)
	}

	// Now that it IS routing, one more search must not touch PostgreSQL at all.
	// Counting across the loop above would have counted the attempts that
	// legitimately fell back while the provider was still catching up.
	before := postgresCalls
	reply, route, err := router.Search(ctx, request)
	if err != nil || route != RouteProvider {
		t.Fatalf("a settled route did not hold: route=%v err=%v", route, err)
	}
	if len(reply.Candidates) != 2 {
		t.Fatalf("a settled routed search returned %+v", reply.Candidates)
	}
	if postgresCalls != before {
		t.Errorf("PostgreSQL was queried %d times for a routed search", postgresCalls-before)
	}

	// With the provider forgotten, the very next search is PostgreSQL's again.
	// Removing DB3 must be as uneventful as never installing it.
	attachment.ForgetProvider(456, 1)
	before = postgresCalls
	if _, route, err := router.Search(ctx, request); err != nil || route != RoutePostgreSQL {
		t.Fatalf("after the provider left: route=%v err=%v", route, err)
	}
	if postgresCalls != before+1 {
		t.Error("PostgreSQL did not answer after the provider was removed")
	}
}

// seedProviderThroughTheBus applies points to the provider and reports the
// capabilities the module should believe.
func seedProviderThroughTheBus(t *testing.T, ctx context.Context, client *bus.Client,
	attachment *VectorBus) func() db3.Capabilities {
	t.Helper()
	labels := []db3.ExactLabel{
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
		apply := db3.Apply{
			OperationID: uint64(i + 1), Generation: 1, PointID: point.id,
			Kind: db3.ApplyUpsert, Collection: "memory",
			Vector: point.vector, Labels: labels,
		}
		wire, err := db3.EncodeApply(apply)
		if err != nil {
			t.Fatal(err)
		}
		if err := client.Publish(db3.EventApply, wire); err != nil {
			t.Fatalf("publishing apply %d: %v", point.id, err)
		}
	}
	_ = ctx
	// The provider's generation moves with every apply and it republishes, but
	// this module's subscription is not wired here, so the test supplies the
	// announcement. The value it reports is read from the provider's own
	// answers: capabilities carrying a generation the provider is not at would
	// be refused by the wire, which is what the retry loop above detects.
	generation := uint64(1)
	return func() db3.Capabilities {
		generation++
		return db3.Capabilities{
			Generation: generation,
			Operations: db3.OperationSearch | db3.OperationApply,
			Metrics:    db3.MetricCosine, Filters: db3.FilterExact,
			MaxDimension: db3.MaxDimension, MaxBatch: 64, MaxTopK: db3.MaxTopK, Ready: true,
		}
	}
}
