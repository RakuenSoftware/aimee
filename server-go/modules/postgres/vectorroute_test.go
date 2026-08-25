package postgres

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/JBailes/aimee/server-go/db3"
)

func routableRequest() db3.SearchRequest {
	return db3.SearchRequest{
		RequestID: 1, RequiredGeneration: 7, Workspace: "w1", Project: "p1",
		RecordType: "memory", TopK: 3, Vector: []float32{1, 0, 0},
	}
}

type fakeSearcher struct {
	calls     int
	principal uint32
	reply     db3.SearchReply
	err       error
}

func (f *fakeSearcher) Search(_ context.Context, principal uint32,
	request db3.SearchRequest) (db3.SearchReply, error) {
	f.calls++
	f.principal = principal
	if f.err != nil {
		return db3.SearchReply{}, f.err
	}
	reply := f.reply
	if reply.RequestID == 0 {
		reply.RequestID = request.RequestID
		reply.Generation = request.RequiredGeneration
	}
	return reply, nil
}

// newRouter builds a router as a deployment would: a provisioned principal, or
// zero for the ordinary case of no vector database.
func newRouter(t *testing.T, principal uint32, searcher ProviderSearcher) (*VectorRouter, *int) {
	t.Helper()
	fallbacks := 0
	router, err := NewVectorRouter(principal, searcher,
		func(_ context.Context, request db3.SearchRequest) (db3.SearchReply, error) {
			fallbacks++
			return db3.SearchReply{
				RequestID: request.RequestID, Generation: request.RequiredGeneration,
				Candidates: []db3.Candidate{{PointID: 99, Score: 0.1}},
			}, nil
		})
	if err != nil {
		t.Fatal(err)
	}
	return router, &fallbacks
}

func TestWithNoVectorDatabaseProvisionedEverythingRunsInDatabase(t *testing.T) {
	// The default deployment. Nothing was provisioned, so nothing is routed --
	// not as a degraded mode, but as the ordinary one.
	router, fallbacks := newRouter(t, 0, nil)
	reply, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RoutePostgreSQL {
		t.Errorf("route = %v, want postgresql", route)
	}
	if *fallbacks != 1 || len(reply.Candidates) != 1 {
		t.Errorf("PostgreSQL did not answer (%d calls, %d candidates)", *fallbacks, len(reply.Candidates))
	}
	if router.Provider() != 0 {
		t.Errorf("Provider() = %d with nothing provisioned", router.Provider())
	}
}

func TestAProvisionedProviderAnswers(t *testing.T) {
	searcher := &fakeSearcher{}
	router, fallbacks := newRouter(t, db3.ProviderRefFirst, searcher)
	_, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteProvider {
		t.Fatalf("route = %v, want provider", route)
	}
	if searcher.calls != 1 || searcher.principal != db3.ProviderRefFirst {
		t.Errorf("provider was asked %d times as principal %d", searcher.calls, searcher.principal)
	}
	if *fallbacks != 0 {
		t.Error("PostgreSQL was queried even though the provider answered")
	}
}

func TestTheRoutingDecisionDoesNotChangeUnderACaller(t *testing.T) {
	// A vector database cannot come and go. Whatever was provisioned at boot is
	// what answers, every time, for the life of the process -- otherwise the
	// same query answers from different stores minutes apart with no way for a
	// caller to know which.
	searcher := &fakeSearcher{}
	router, _ := newRouter(t, db3.ProviderRefFirst, searcher)
	for i := 0; i < 16; i++ {
		_, route, err := router.Search(context.Background(), routableRequest())
		if err != nil || route != RouteProvider {
			t.Fatalf("attempt %d: route=%v err=%v", i, route, err)
		}
	}
	if router.Provider() != db3.ProviderRefFirst {
		t.Errorf("Provider() moved to %d", router.Provider())
	}
}

func TestAFailingProviderFallsBackButStaysTheRoute(t *testing.T) {
	// An empty result is indistinguishable from a corpus with no matches, so a
	// broken provider must not become one. It also must not stop being the
	// route: quietly demoting it hides the deployment problem an operator needs
	// to see, and makes the store an answer comes from depend on when it was
	// asked.
	searcher := &fakeSearcher{err: errors.New("provider is down")}
	router, fallbacks := newRouter(t, db3.ProviderRefFirst, searcher)
	reply, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteFellBack {
		t.Fatalf("route = %v, want fell-back", route)
	}
	if *fallbacks != 1 || len(reply.Candidates) != 1 {
		t.Error("PostgreSQL did not answer after the provider failed")
	}
	if router.Provider() != db3.ProviderRefFirst {
		t.Error("a failing provider was demoted; the deployment silently changed")
	}
}

func TestAMalformedProviderReplyFallsBack(t *testing.T) {
	// A reply the wire refuses is not an answer. Accepting it would let a
	// provider return candidates for a different request.
	searcher := &fakeSearcher{reply: db3.SearchReply{RequestID: 999, Generation: 7}}
	router, fallbacks := newRouter(t, db3.ProviderRefFirst, searcher)
	_, route, err := router.Search(context.Background(), routableRequest())
	if err != nil {
		t.Fatal(err)
	}
	if route != RouteFellBack || *fallbacks != 1 {
		t.Errorf("a malformed reply was accepted (route=%v fallbacks=%d)", route, *fallbacks)
	}
}

func TestFellBackIsDistinctFromNeverRouted(t *testing.T) {
	// "No vector database installed" and "the one installed is failing" look
	// identical in the results and could not be more different operationally.
	searcher := &fakeSearcher{err: errors.New("down")}
	failing, _ := newRouter(t, db3.ProviderRefFirst, searcher)
	_, failedRoute, _ := failing.Search(context.Background(), routableRequest())

	absent, _ := newRouter(t, 0, nil)
	_, absentRoute, _ := absent.Search(context.Background(), routableRequest())

	if failedRoute == absentRoute {
		t.Fatalf("both reported %v; a deployment cannot tell a broken provider from none", failedRoute)
	}
}

func TestAnOperationTheWireCannotCarryStaysInDatabase(t *testing.T) {
	// Not every vector operation can leave. A request the contract refuses is
	// not a failure; it is an operation that runs where it always did.
	searcher := &fakeSearcher{}
	router, fallbacks := newRouter(t, db3.ProviderRefFirst, searcher)

	unroutable := routableRequest()
	unroutable.TopK = 0
	if RoutableSearch(unroutable) {
		t.Fatal("a request with no top-k was judged routable")
	}
	_, route, _ := router.Search(context.Background(), unroutable)
	if route != RoutePostgreSQL || searcher.calls != 0 || *fallbacks != 1 {
		t.Errorf("an unroutable request reached a provider (route=%v calls=%d)", route, searcher.calls)
	}
}

func TestACancelledContextDoesNotSpendPostgreSQL(t *testing.T) {
	// The caller has gone. Running the query anyway would spend the database on
	// an answer nobody is waiting for.
	searcher := &fakeSearcher{err: errors.New("down")}
	router, fallbacks := newRouter(t, db3.ProviderRefFirst, searcher)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, _, err := router.Search(ctx, routableRequest()); err == nil {
		t.Fatal("a cancelled search reported success")
	}
	if *fallbacks != 0 {
		t.Errorf("PostgreSQL was queried for a cancelled caller (%d times)", *fallbacks)
	}
}

func TestARouterWithoutAnInDatabasePathIsRefused(t *testing.T) {
	// PostgreSQL is never optional; a vector database always is. A router built
	// the other way round would make an optional component load-bearing.
	if _, err := NewVectorRouter(db3.ProviderRefFirst, &fakeSearcher{}, nil); err == nil {
		t.Fatal("a router was built with no in-database fallback")
	}
}

// --- what the deployment provisioned -------------------------------------

func writeGrant(t *testing.T, dir, name, body string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestNoGrantMeansNoVectorDatabase(t *testing.T) {
	// The ordinary deployment, and never an error.
	provider, err := ProvisionedVectorProvider(t.TempDir())
	if err != nil || provider.Principal != 0 {
		t.Fatalf("empty policy dir gave (%+v, %v)", provider, err)
	}
	// A directory that does not exist is the same answer: nothing was
	// provisioned. A deployment without a policy directory has installed no
	// modules at all.
	provider, err = ProvisionedVectorProvider(filepath.Join(t.TempDir(), "absent"))
	if err != nil || provider.Principal != 0 {
		t.Fatalf("missing policy dir gave (%+v, %v)", provider, err)
	}
	if provider, err := ProvisionedVectorProvider(""); err != nil || provider.Principal != 0 {
		t.Fatalf("unset policy dir gave (%+v, %v)", provider, err)
	}
}

func TestAProvisionedGrantIsFound(t *testing.T) {
	dir := t.TempDir()
	writeGrant(t, dir, "db3-qdrant.grant",
		"version=1\nprincipal_class=1\nprincipal_ref=456\nuid=self\n"+
			"executable=/usr/local/libexec/aimee-modules/aimee-module\n")
	provider, err := ProvisionedVectorProvider(dir)
	if err != nil {
		t.Fatal(err)
	}
	if provider.Principal != 456 || provider.Instance != "qdrant" {
		t.Fatalf("provider = %+v, want principal 456 instance qdrant", provider)
	}
}

func TestGrantsOutsideTheProviderBandAreIgnored(t *testing.T) {
	// The band is what keeps a provider from deriving a canonical module's
	// event kinds. A grant naming a ref outside it was hand-edited or written
	// by a provisioner predating the band, and neither is something to route
	// vector traffic through.
	dir := t.TempDir()
	writeGrant(t, dir, "db3-rogue.grant",
		"version=1\nprincipal_ref=28\nexecutable=/usr/local/bin/x\n")
	provider, err := ProvisionedVectorProvider(dir)
	if err != nil || provider.Principal != 0 {
		t.Fatalf("an out-of-band grant was accepted: (%+v, %v)", provider, err)
	}
}

func TestNonProviderGrantsAreNotMistakenForOne(t *testing.T) {
	dir := t.TempDir()
	writeGrant(t, dir, "mcp-github.grant", "version=1\nprincipal_ref=200\n")
	writeGrant(t, dir, "notes.txt", "principal_ref=456\n")
	provider, err := ProvisionedVectorProvider(dir)
	if err != nil || provider.Principal != 0 {
		t.Fatalf("a non-provider file was read as a provider grant: (%+v, %v)", provider, err)
	}
}

func TestTwoProvisionedProvidersAreRefusedRatherThanChosenBetween(t *testing.T) {
	// Only one may serve the search kind -- the bus binds a kind to exactly one
	// slot -- so a second grant is a misconfiguration. Picking one silently
	// would leave the operator with a provider that is installed, granted, and
	// never used.
	dir := t.TempDir()
	writeGrant(t, dir, "db3-qdrant.grant", "version=1\nprincipal_ref=456\n")
	writeGrant(t, dir, "db3-milvus.grant", "version=1\nprincipal_ref=457\n")
	provider, err := ProvisionedVectorProvider(dir)
	if err == nil {
		t.Fatalf("two provisioned providers were accepted: %+v", provider)
	}
	if provider.Principal != 0 {
		t.Error("a provider was selected despite the misconfiguration")
	}
}
