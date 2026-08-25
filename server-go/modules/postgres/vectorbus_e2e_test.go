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

	// The postgres module attaches as ITSELF. Principal 28 is postgres, and it
	// is the only principal the harness grants the caller role -- because it is
	// the only one a deployment gives a real grant to speak to a vector store.
	// Every other DB operation reaches the store by going to postgres first.
	client := attachClient(t, socket, 28)
	defer client.Detach()

	// The deployment, as an operator provisions it: a grant naming the provider
	// this test started. The module reads it once and that is the answer for
	// the rest of its life.
	policyDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(policyDir, "db3-pgroute.grant"),
		[]byte("version=1\nprincipal_class=1\nprincipal_ref=456\nuid=self\n"+
			"executable="+provider+"\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := ProvisionedVectorProvider(policyDir)
	if err != nil {
		t.Fatal(err)
	}
	if provisioned.Principal != 456 {
		t.Fatalf("the grant was not read: %+v", provisioned)
	}

	// A fallback that records whether PostgreSQL was asked. That is the whole
	// question: a routed search must not touch it, and every other case must.
	postgresCalls := 0
	attachment, err := AttachVectorBus(client, provisioned)
	if err != nil {
		t.Fatal(err)
	}
	defer attachment.Close()
	router, err := NewVectorRouter(provisioned.Principal, attachment.Searcher(),
		func(_ context.Context, request db3.SearchRequest) (db3.SearchReply, error) {
			postgresCalls++
			return db3.SearchReply{
				RequestID: request.RequestID, Generation: request.RequiredGeneration,
			}, nil
		})
	if err != nil {
		t.Fatal(err)
	}

	// The caller no longer polls: whoever owns the attachment does. In the
	// module process that is the module loop; here the test owns the client, so
	// the test pumps it. This is the single-reader rule made visible -- there is
	// exactly one Poll on a client, and everything else is fed from it.
	pumpDone := make(chan struct{})
	go func() {
		defer close(pumpDone)
		for {
			select {
			case <-ctx.Done():
				return
			default:
			}
			client.HeartbeatNow()
			event, ok, err := client.Poll()
			if err != nil {
				return
			}
			if !ok {
				time.Sleep(time.Millisecond)
				continue
			}
			attachment.Absorb(event)
		}
	}()
	defer func() { cancel(); <-pumpDone }()

	request := db3.SearchRequest{
		RequestID: 1, RequiredGeneration: 1, Workspace: "workspace-a",
		Project: "project-a", RecordType: "memory", TopK: 2, Vector: []float32{1, 0, 0},
	}

	// The grant said a provider is installed, so the router routes from the
	// first call. It does not wait to observe anything: the deployment is a
	// fact, not something to be discovered.
	//
	// The generation is PostgreSQL's. It stamps every apply with it and searches
	// at the same value, and the provider carries back whatever it was told --
	// which is what makes the two agree without either discovering the other.
	//
	// Applies are re-published each attempt because a publish is not queued for
	// a subscriber that has not arrived yet: one sent before the provider is
	// serving is simply lost. Re-sending is safe -- the provider recognises an
	// operation id it has already applied and acknowledges it without repeating
	// the work -- so this converges rather than needing the two starts ordered.
	deadline = time.Now().Add(30 * time.Second)
	var routed db3.SearchReply
	for {
		seedProviderThroughTheBus(t, ctx, client)
		reply, route, err := router.Search(ctx, request)
		if err == nil && route == RouteProvider && len(reply.Candidates) == 2 {
			routed = reply
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("the provider never returned the applied points "+
				"(postgres calls %d, route %v, err %v)", postgresCalls, route, err)
		}
		time.Sleep(100 * time.Millisecond)
	}

	if routed.Candidates[0].PointID != 41 || routed.Candidates[1].PointID != 42 {
		t.Fatalf("routed candidates = %+v, want 41 then 42", routed.Candidates)
	}

	// A routed search must not touch PostgreSQL at all.
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

	// And the decision does not drift. The provider provisioned at boot is the
	// one that answers, every time, for the life of this process.
	if router.Provider() != 456 {
		t.Errorf("the routing decision moved to %d", router.Provider())
	}
}

// seedProviderThroughTheBus applies points into the provider over the wire.
func seedProviderThroughTheBus(t *testing.T, ctx context.Context, client *bus.Client) {
	t.Helper()
	_ = ctx
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
		wire, err := db3.EncodeApply(db3.Apply{
			OperationID: uint64(i + 1), Generation: 1, PointID: point.id,
			Kind: db3.ApplyUpsert, Collection: "memory",
			Vector: point.vector, Labels: labels,
		})
		if err != nil {
			t.Fatal(err)
		}
		if err := client.Publish(db3.EventApply, wire); err != nil {
			t.Fatalf("publishing apply %d: %v", point.id, err)
		}
	}
}
