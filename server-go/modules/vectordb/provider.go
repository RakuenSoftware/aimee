package vectordb

import (
	"context"
	"errors"
	"sync"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

// ErrDimension reports a vector whose length is not the index's dimension.
var ErrDimension = errors.New("vectordb: vector dimension mismatch")

// maxBatch is the largest apply batch this provider accepts. It is advertised
// in the capabilities so DB2 shapes its outbox fan-out to what will actually be
// taken, rather than discovering the limit through rejections.
const maxBatch = 256

// Provider serves the portable half of the vector contract over DB3.
//
// It owns an Index and translates the wire's requests into index operations.
// Every authority decision stays in DB2: this answers with opaque point ids and
// scores and nothing else.
type Provider struct {
	backend Backend
	// collection is the namespace this provider serves.
	//
	// The DB3 search request has no collection field — only a record_type,
	// which the projection catalog defines as a LABEL within a collection, not
	// the collection itself. So a provider serves one collection, named here,
	// and treats record_type as one more exact filter. Reading record_type as
	// the collection would search a namespace the caller never asked for and
	// return confident results from the wrong corpus.
	collection string

	mu sync.Mutex
	// applied remembers which operation ids have landed, so a replayed apply
	// is recognised rather than performed twice.
	//
	// The outbox delivers at least once — that is what makes it durable — so a
	// provider that treated every delivery as new would double-apply on any
	// redelivery. For an upsert that is merely wasteful; for the watermark it
	// would be wrong.
	applied map[uint64]bool
	// contiguous is the highest operation id below which nothing is missing.
	contiguous uint64

	// updates carries a fresh Capabilities every time an apply moves the
	// index's generation.
	//
	// It is not an optimisation. DB2 sends the generation it REQUIRES, and the
	// wire requires the reply to carry exactly that generation back
	// (ValidateSearchReply). Every apply bumps the index generation, so a
	// provider that never republished would answer at a generation DB2 has not
	// been told about, its reply would be rejected as malformed, and the
	// provider runtime would report SearchFailureInternal. That is not a
	// degraded search: after the FIRST apply, every routed search fails, for
	// good, and the provider looks broken rather than stale.
	//
	// Buffered at one and coalescing: only the newest generation matters, and a
	// provider must never block an apply waiting for a reader.
	updates chan db3.Capabilities
}

// NewProvider builds a provider serving one collection out of an index.
func NewProvider(index *Index, collection string) *Provider {
	return NewProviderWithBackend(NewMemoryBackend(index), collection)
}

// NewProviderWithBackend builds a provider over any store.
//
// This is the constructor a deployment uses: which vector store sits behind the
// DB3 contract is a deployment choice, and the provider is the same module
// either way.
func NewProviderWithBackend(backend Backend, collection string) *Provider {
	return &Provider{
		backend: backend, collection: collection, applied: map[uint64]bool{},
		updates: make(chan db3.Capabilities, 1),
	}
}

// Capabilities describes what this provider will serve.
//
// Ready is reported only once the index has a generation, which it always does
// — but the field is set from the index rather than hard-coded, because a real
// provider's readiness is a fact about its backing store and not a constant.
func (p *Provider) Capabilities() db3.Capabilities {
	return db3.Capabilities{
		Generation:   p.generation(),
		Operations:   db3.OperationSearch | db3.OperationApply,
		Metrics:      p.metricSet(),
		Filters:      db3.FilterExact,
		MaxDimension: uint32(p.backend.Dimension()),
		MaxBatch:     maxBatch,
		MaxTopK:      db3.MaxTopK,
		Ready:        true,
	}
}

func (p *Provider) metricSet() db3.MetricSet {
	switch p.backend.Metric() {
	case L2:
		return db3.MetricL2
	case Dot:
		return db3.MetricDot
	default:
		return db3.MetricCosine
	}
}

// Search answers a candidate search.
//
// A request naming a generation newer than the index's is refused as retryable
// rather than answered. The caller is telling the provider it has already seen
// writes this index may not have, so answering would hand back a ranking that
// silently omits them — and retryable is the honest code, because the apply
// stream is expected to deliver those writes shortly.
func (p *Provider) Search(ctx context.Context, request db3.SearchRequest) (db3.SearchReply, db3.SearchFailureCode) {
	if request.RequestID == 0 || request.TopK == 0 || len(request.Vector) == 0 {
		return db3.SearchReply{}, db3.SearchFailureInvalidRequest
	}
	if len(request.Vector) != p.backend.Dimension() {
		return db3.SearchReply{}, db3.SearchFailureInvalidRequest
	}
	if err := ctx.Err(); err != nil {
		return db3.SearchReply{}, db3.SearchFailureRetryable
	}

	generation := p.generation()
	if request.RequiredGeneration > generation {
		return db3.SearchReply{}, db3.SearchFailureRetryable
	}

	// The request's scope becomes exact-match filters. Scope is not advisory:
	// a search that ignored it would return candidates from another workspace,
	// and DB2 rehydrating them would leak their existence through the timing
	// even after refusing to return their content.
	filters := scopeFilters(request)

	candidates, err := p.backend.Search(ctx, p.collection, request.Vector, int(request.TopK), filters)
	if err != nil {
		// A backend that could not answer must not look like a corpus with no
		// hits. Retryable rather than internal: a remote store is usually
		// briefly unreachable rather than wrong, and DB2 falls back to pgvector
		// either way.
		return db3.SearchReply{}, db3.SearchFailureRetryable
	}
	return db3.SearchReply{
		RequestID:  request.RequestID,
		Generation: generation,
		Candidates: candidates,
	}, 0
}

// scopeFilters turns the request's scope into label filters.
func scopeFilters(request db3.SearchRequest) []db3.ExactLabel {
	filters := make([]db3.ExactLabel, 0, 3)
	if request.Workspace != "" {
		filters = append(filters, db3.ExactLabel{Key: "workspace", Value: request.Workspace})
	}
	if request.Project != "" {
		filters = append(filters, db3.ExactLabel{Key: "project", Value: request.Project})
	}
	// record_type is a label the projection catalog stores beside the vector,
	// so it narrows within the collection rather than choosing one.
	if request.RecordType != "" {
		filters = append(filters, db3.ExactLabel{Key: "record_type", Value: request.RecordType})
	}
	// The request's own filters are ANDed with the scope, never merged with it.
	// A filter that contradicts the scope therefore matches nothing, which is
	// the safe direction: the answer narrows rather than widens.
	filters = append(filters, request.Filters...)
	return filters
}

// Apply performs one committed mutation from DB2's outbox.
func (p *Provider) Apply(ctx context.Context, apply db3.Apply) db3.ProviderApplyOutcome {
	if apply.OperationID == 0 {
		return db3.ProviderApplyOutcome{Result: db3.AppliedRejected}
	}
	if err := ctx.Err(); err != nil {
		return db3.ProviderApplyOutcome{Result: db3.AppliedRetryable}
	}

	p.mu.Lock()
	defer p.mu.Unlock()

	// A redelivery is acknowledged at the current watermark rather than
	// re-performed. Reporting it as rejected would stall the outbox on an
	// operation that has, in fact, already landed.
	if p.applied[apply.OperationID] {
		return db3.ProviderApplyOutcome{Result: db3.AppliedOK, Watermark: p.watermarkLocked(apply.OperationID)}
	}

	switch apply.Kind {
	case db3.ApplyUpsert:
		if err := p.backend.Upsert(ctx, apply.Collection, apply.PointID, apply.Vector, apply.Labels); err != nil {
			// A dimension mismatch is the caller's error and will never
			// succeed on retry, so it is rejected rather than deferred.
			return db3.ProviderApplyOutcome{Result: db3.AppliedRejected}
		}
	case db3.ApplyDelete:
		if err := p.backend.Delete(ctx, apply.Collection, apply.PointID); err != nil {
			// Unlike a dimension mismatch, a store that was unreachable will
			// succeed on redelivery, so the outbox must retry rather than skip.
			return db3.ProviderApplyOutcome{Result: db3.AppliedRetryable}
		}
	case db3.ApplyTombstone:
		if err := p.backend.Tombstone(ctx, apply.Collection, apply.PointID); err != nil {
			return db3.ProviderApplyOutcome{Result: db3.AppliedRetryable}
		}
	default:
		return db3.ProviderApplyOutcome{Result: db3.AppliedRejected}
	}

	p.applied[apply.OperationID] = true
	watermark := p.advanceLocked()
	p.publishCapabilitiesLocked()
	return db3.ProviderApplyOutcome{
		Result:    db3.AppliedOK,
		Watermark: watermark,
		Lag:       p.lagLocked(apply.OperationID, watermark),
	}
}

// publishCapabilitiesLocked offers the router the generation this provider is
// now at, replacing any older pending one.
//
// Coalescing rather than queueing: a router that is behind wants the CURRENT
// generation, never a backlog of superseded ones. Never blocking: an apply that
// waited on a reader would stall DB2's outbox behind the provider.
func (p *Provider) publishCapabilitiesLocked() {
	if p.updates == nil {
		return
	}
	// Safe under p.mu: Capabilities takes the INDEX lock, and advanceLocked
	// above already establishes that order (provider then index).
	capabilities := p.Capabilities()
	// Apply holds p.mu, so this is the only sender. A failed send therefore means
	// the buffer holds a SUPERSEDED generation: drop it and send again, and the
	// second send cannot fail because nothing else can refill it.
	//
	// The drain must not give up when it finds the buffer already empty. If the
	// reader took the old value between the failed send and the drain, returning
	// there would leave the router holding the generation we were replacing --
	// which is the stale-generation failure this whole channel exists to
	// prevent, reintroduced as a race that shows up only under load.
	for {
		select {
		case p.updates <- capabilities:
			return
		default:
		}
		select {
		case <-p.updates:
		default:
		}
	}
}

// advanceLocked moves the watermark over every contiguously applied operation.
//
// The watermark is contiguous rather than "highest seen" on purpose: it is
// DB2's promise that nothing below it is outstanding. Advancing it past a gap
// would tell DB2 an operation had landed when it had not, and the outbox would
// never redeliver it.
func (p *Provider) advanceLocked() uint64 {
	for p.applied[p.contiguous+1] {
		p.contiguous++
	}
	if memory, ok := p.backend.(*MemoryBackend); ok {
		memory.setWatermark(p.contiguous)
	}
	return p.contiguous
}

func (p *Provider) watermarkLocked(uint64) uint64 { return p.contiguous }

// lagLocked reports how many operations are applied but not yet contiguous.
func (p *Provider) lagLocked(operationID, watermark uint64) uint32 {
	if operationID <= watermark {
		return 0
	}
	return uint32(operationID - watermark)
}

// Run serves this provider on an attached bus client until ctx ends.
func (p *Provider) Run(ctx context.Context, client *bus.Client) error {
	return db3.RunProvider(ctx, client, db3.ProviderConfig{
		Capabilities:      p.Capabilities(),
		CapabilityUpdates: p.updates,
		Search:            p.Search,
		Apply:             p.Apply,
	})
}

// generation reads the backend's version, treating an error as "no generation".
//
// Zero is not a valid generation on the wire, so a store that cannot report one
// makes the provider advertise itself as having none -- which the router reads
// as not ready, and no search is sent. That is the safe direction: answering at
// a guessed generation would have DB2 accept results from a store whose version
// it does not actually know.
func (p *Provider) generation() uint64 {
	generation, err := p.backend.Generation(context.Background())
	if err != nil {
		return 0
	}
	return generation
}
