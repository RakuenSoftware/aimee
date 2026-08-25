//go:build linux

package db2

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	protocol "github.com/JBailes/aimee/server-go/db3"
	"golang.org/x/sys/unix"
)

func attachDB3Client(t *testing.T, socket string, principal uint32) *bus.Client {
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

func routeOverBus(t *testing.T, client *bus.Client, correlation uint64,
	request protocol.RouteRequest) protocol.RouteReply {
	t.Helper()
	wire, err := protocol.EncodeRouteRequest(request)
	if err != nil {
		t.Fatal(err)
	}
	if err := client.Request(protocol.EventRoute, correlation, wire); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(3 * time.Second)
	var body []byte
	for time.Now().Before(deadline) {
		client.HeartbeatNow()
		event, ok, err := client.Poll()
		if err != nil {
			t.Fatal(err)
		}
		if !ok {
			time.Sleep(time.Millisecond)
			continue
		}
		if event.Frame.EventKind != protocol.EventRoute ||
			event.Frame.CorrelationID != correlation || event.Frame.HdrFlags&bus.FReply == 0 {
			continue
		}
		body = append(body, event.Payload...)
		if event.Frame.HdrFlags&bus.FMore != 0 {
			continue
		}
		reply, err := protocol.DecodeRouteReply(body)
		if err != nil {
			t.Fatal(err)
		}
		return reply
	}
	t.Fatal("timed out waiting for DB3 route reply")
	return protocol.RouteReply{}
}

// The principals the two providers attach as.
//
// They are DERIVED from the reserved band rather than written out, because a
// provider outside it is refused: ObserveCapabilities calls ValidateProviderRef
// before it will route to anyone. These were once literal 1001 and 1002, which
// were valid until the provider band was introduced and then silently were not
// -- the router refused both, no route ever deployed, and the test failed on a
// route query with nothing to say why. Nothing in CI runs this test, so it went
// on failing. Deriving them means moving the band moves the test with it.
//
// providerBPrincipal must stay a DIFFERENT ref in the same band: the test's
// whole point is that two providers are told apart, so collapsing them to one
// would let a broken router pass.
const (
	providerAPrincipal = protocol.ProviderRefFirst
	providerBPrincipal = protocol.ProviderRefFirst + 1
	// The control client only requests routes; it is not a provider and so is
	// not band-checked. It stays outside the band on purpose, to keep it out of
	// the space a provisioned provider draws from.
	controlPrincipal = 1003
)

func TestDB3GoProvidersOperateOverAuthenticatedCBus(t *testing.T) {
	harness := os.Getenv("DB3_GO_HOST")
	if harness == "" {
		t.Skip("DB3_GO_HOST is unset; run scripts/test_db3_go_bus.sh")
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	directory := t.TempDir()
	socket := filepath.Join(directory, "db3.sock")
	command := exec.Command(harness, socket, executable)
	var output bytes.Buffer
	command.Stdout, command.Stderr = &output, &output
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = command.Process.Signal(unix.SIGTERM)
		_ = command.Wait()
		if t.Failed() {
			t.Logf("DB3 C host output:\n%s", output.String())
		}
	}()
	deadline := time.Now().Add(3 * time.Second)
	for {
		if _, err := os.Stat(socket); err == nil {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("DB3 host socket did not appear")
		}
		time.Sleep(5 * time.Millisecond)
	}

	db2Client := attachDB3Client(t, socket, 28)
	providerAClient := attachDB3Client(t, socket, providerAPrincipal)
	providerBClient := attachDB3Client(t, socket, providerBPrincipal)
	controlClient := attachDB3Client(t, socket, controlPrincipal)
	defer controlClient.Detach()
	defer providerBClient.Detach()
	defer providerAClient.Detach()
	defer db2Client.Detach()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var internalCalls atomic.Int32
	acknowledgements := make(chan uint32, 8)
	router, endpoint, err := NewDB3BusRouter(ctx, db2Client,
		func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, error) {
			internalCalls.Add(1)
			return db3Reply(request, 90), nil
		},
		func(_ context.Context, _, _ string, pointID int64) (bool, error) {
			return pointID != 42, nil
		},
		func(principal uint32, _ protocol.Applied) { acknowledgements <- principal })
	if err != nil {
		t.Fatal(err)
	}

	type applyState struct {
		mu         sync.Mutex
		operation  uint64
		generation uint64
		effects    int
		duplicates int
	}
	applyHandler := func(state *applyState) protocol.ProviderApplyFunc {
		return func(_ context.Context, apply protocol.Apply) protocol.ProviderApplyOutcome {
			state.mu.Lock()
			if state.operation == apply.OperationID && state.generation == apply.Generation {
				state.duplicates++
			} else {
				state.operation, state.generation = apply.OperationID, apply.Generation
				state.effects++
			}
			state.mu.Unlock()
			return protocol.ProviderApplyOutcome{Result: protocol.AppliedOK, Watermark: apply.OperationID}
		}
	}
	stateA, stateB := &applyState{}, &applyState{}
	updatesA := make(chan protocol.Capabilities, 2)
	providerDone := make(chan error, 2)
	go func() {
		providerDone <- protocol.RunProvider(ctx, providerAClient, protocol.ProviderConfig{
			Capabilities: db3Capabilities(7, true), CapabilityUpdates: updatesA,
			Search: func(_ context.Context, request protocol.SearchRequest) (protocol.SearchReply, protocol.SearchFailureCode) {
				return protocol.SearchReply{RequestID: request.RequestID, Generation: request.RequiredGeneration,
					Candidates: []protocol.Candidate{{PointID: 41, Score: .95}, {PointID: 42, Score: .75}}}, 0
			},
			Apply: applyHandler(stateA),
		})
	}()
	providerBCapabilities := protocol.Capabilities{
		Generation: 7, Operations: protocol.OperationApply, MaxBatch: 64, Ready: true,
	}
	go func() {
		providerDone <- protocol.RunProvider(ctx, providerBClient, protocol.ProviderConfig{
			Capabilities: providerBCapabilities, Apply: applyHandler(stateB),
		})
	}()

	var route protocol.RouteReply
	for attempt := uint64(1); attempt <= 100; attempt++ {
		route = routeOverBus(t, controlClient, attempt, protocol.RouteRequest{
			RequestID: attempt, Action: protocol.RouteQuery,
		})
		if route.Result == protocol.RouteOK && route.SelectedPrincipal == providerAPrincipal {
			break
		}
		time.Sleep(2 * time.Millisecond)
	}
	if route.Result != protocol.RouteOK || route.SelectedPrincipal != providerAPrincipal {
		t.Fatalf("automatic deployed route = %+v", route)
	}

	outcome := router.Search(context.Background(), db3Request())
	if outcome.Result != DB3OK || outcome.Route != DB3External ||
		len(outcome.Reply.Candidates) != 1 || outcome.Reply.Candidates[0].PointID != 41 ||
		internalCalls.Load() != 0 {
		t.Fatalf("external search = %+v, internal calls %d", outcome, internalCalls.Load())
	}
	// Deployment chooses the default without fallback. Control may then pin the
	// same provider with explicit fallback for the readiness-loss case below.
	route = routeOverBus(t, controlClient, 101, protocol.RouteRequest{
		RequestID: 101, Action: protocol.RouteSelect, Principal: providerAPrincipal,
		CapabilityGeneration: 7, Fallback: true,
	})
	if route.Result != protocol.RouteOK || route.SelectedPrincipal != providerAPrincipal || !route.Fallback {
		t.Fatalf("explicit fallback route = %+v", route)
	}

	apply := protocol.Apply{OperationID: 7001, Generation: 7, PointID: 41,
		Kind: protocol.ApplyUpsert, Collection: "memory", Vector: make([]float32, 40)}
	for i := range apply.Vector {
		apply.Vector[i] = float32(i) / 100
	}
	if err := endpoint.PublishApply(context.Background(), apply); err != nil {
		t.Fatal(err)
	}
	if err := endpoint.PublishApply(context.Background(), apply); err != nil {
		t.Fatal(err)
	}
	counts := map[uint32]int{}
	for len(counts) < 2 || counts[providerAPrincipal] < 2 || counts[providerBPrincipal] < 2 {
		select {
		case principal := <-acknowledgements:
			counts[principal]++
		case <-time.After(3 * time.Second):
			t.Fatalf("acknowledgements = %v", counts)
		}
	}
	for principal, state := range map[uint32]*applyState{providerAPrincipal: stateA, providerBPrincipal: stateB} {
		state.mu.Lock()
		if state.effects != 1 || state.duplicates != 1 {
			t.Fatalf("provider %d state = %+v", principal, state)
		}
		state.mu.Unlock()
	}

	unready := db3Capabilities(7, false)
	updatesA <- unready
	deadline = time.Now().Add(3 * time.Second)
	for {
		outcome = router.Search(context.Background(), db3Request())
		if outcome.Route == DB3ExplicitFallback {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("provider readiness did not reach router; last outcome %+v", outcome)
		}
		time.Sleep(2 * time.Millisecond)
	}
	if outcome.Result != DB3OK || outcome.ExternalError != DB3Unavailable ||
		outcome.Reply.Candidates[0].PointID != 90 || internalCalls.Load() == 0 {
		t.Fatalf("explicit fallback = %+v, internal calls %d", outcome, internalCalls.Load())
	}

	cancel()
	for i := 0; i < 2; i++ {
		if err := <-providerDone; err != nil && !errors.Is(err, context.Canceled) {
			t.Fatalf("provider exit: %v", err)
		}
	}
	waitDB3(t, func() bool { return endpoint.Err() != nil })
}
