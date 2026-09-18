package memory

import (
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestIngressTaskHistoryBothPlacements(t *testing.T) {
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		handler := NewHandler(nil, WithDataStore(placement, nil))
		claim := func(query string, want bool) {
			t.Helper()
			args := fmt.Sprintf(`{"operation":"ingress-task-claim","session":"s","project":"p","query":%q}`, query)
			if result := runHostRuntime(t, handler, args); result["fetch"] != want {
				t.Fatal(query, result)
			}
		}
		claim("fix local resolver", true)
		claim("please fix the local resolver", false)
		claim("document billing retry policy", true)
		claim("document billing retry policy", false)
		runHostRuntime(t, handler, `{"operation":"ingress-task-rearm","session":"s","project":"other"}`)
		claim("document billing retry policy", false)
		runHostRuntime(t, handler, `{"operation":"ingress-task-rearm","session":"s","project":"p"}`)
		claim("document billing retry policy", true)
		runHostRuntime(t, handler, `{"operation":"ingress-task-reset"}`)
		claim("document billing retry policy", true)
		for _, operation := range []string{"claim", "rearm", "reset"} {
			frame, _ := bus.EncodeCommand("runtime", []byte(fmt.Sprintf(`{"operation":"ingress-task-%s","session":"s","project":"p","query":"q"}`, operation)))
			if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 200}, frame); status != bus.ModuleStatusInvalidRequest {
				t.Fatal(operation, status)
			}
		}
	}
}

func TestIngressTaskClaimsAreAtomicAndIsolated(t *testing.T) {
	var state ingressTaskState
	var fetched atomic.Int64
	var wg sync.WaitGroup
	for i := 0; i < 128; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if state.claim("session", "project", "fix local resolver") {
				fetched.Add(1)
			}
		}()
	}
	wg.Wait()
	if fetched.Load() != 1 || !state.claim("session", "other-project", "fix local resolver") ||
		!state.claim("other-session", "project", "fix local resolver") {
		t.Fatal("duplicate claim or mixed scopes", fetched.Load())
	}
	if state.claim("", "project", "query") || state.claim("session", "", "query") {
		t.Fatal("missing identity claimed a packet")
	}
	// Full project identities avoid the old fixed native buffer truncation.
	prefix := strings.Repeat("p", 300)
	if !state.claim("s", prefix+"1", "fix resolver") || !state.claim("s", prefix+"2", "fix resolver") ||
		state.claim("s", prefix+"1", "fix resolver") {
		t.Fatal("long project identity lost")
	}
	var other ingressTaskState
	if !other.claim("session", "project", "fix local resolver") {
		t.Fatal("state leaked between handlers")
	}
}

func TestIngressTaskLRUAndTokenSemantics(t *testing.T) {
	var state ingressTaskState
	for i := 0; i < 64; i++ {
		if !state.claim(fmt.Sprint(i), "p", "query") {
			t.Fatal(i)
		}
	}
	if state.claim("0", "p", "query") || !state.claim("64", "p", "query") || state.claim("0", "p", "query") ||
		!state.claim("1", "p", "query") {
		t.Fatal("LRU did not preserve the recently used entry")
	}
	if ingressTaskTokens("a b !") != 1 || ingressTaskTokens("Fix LOCAL_resolver") != ingressTaskTokens("fix local_RESOLVER") ||
		ingressTaskTokens("fix-local") != ingressTaskTokens("fix local") {
		t.Fatal("ASCII token normalization changed")
	}
}

func TestIngressTaskInvalidArgumentsDoNotClaim(t *testing.T) {
	handler := NewHandler(nil, WithDataStore(PlacementServer, nil))
	for _, args := range []string{
		`{"operation":"ingress-task-claim","session":null,"project":"p","query":"q"}`,
		`{"operation":"ingress-task-claim","session":"s","project":"p","query":null}`,
		`{"operation":"ingress-task-claim","session":"s","project":7,"query":"q"}`,
		`{"operation":"ingress-task-rearm","session":"s"}`,
	} {
		frame, _ := bus.EncodeCommand("runtime", []byte(args))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(args, status)
		}
	}
	result := runHostRuntime(t, handler, `{"operation":"ingress-task-claim","session":"s","project":"p","query":"q"}`)
	if result["fetch"] != true {
		t.Fatal(result)
	}
}
