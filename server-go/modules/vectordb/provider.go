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
	index *Index
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
}

// NewProvider builds a provider serving one collection out of an index.
func NewProvider(index *Index, collection string) *Provider {
	return &Provider{index: index, collection: collection, applied: map[uint64]bool{}}
}

// Capabilities describes what this provider will serve.
//
// Ready is reported only once the index has a generation, which it always does
// — but the field is set from the index rather than hard-coded, because a real
// provider's readiness is a fact about its backing store and not a constant.
func (p *Provider) Capabilities() db3.Capabilities {
	return db3.Capabilities{
		Generation:   p.index.Generation(),
		Operations:   db3.OperationSearch | db3.OperationApply,
		Metrics:      p.metricSet(),
		Filters:      db3.FilterExact,
		MaxDimension: uint32(p.index.dimension),
		MaxBatch:     maxBatch,
		MaxTopK:      db3.MaxTopK,
		Ready:        true,
	}
}

func (p *Provider) metricSet() db3.MetricSet {
	switch p.index.metric {
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
	if len(request.Vector) != p.index.dimension {
		return db3.SearchReply{}, db3.SearchFailureInvalidRequest
	}
	if err := ctx.Err(); err != nil {
		return db3.SearchReply{}, db3.SearchFailureRetryable
	}

	generation := p.index.Generation()
	if request.RequiredGeneration > generation {
		return db3.SearchReply{}, db3.SearchFailureRetryable
	}

	// The request's scope becomes exact-match filters. Scope is not advisory:
	// a search that ignored it would return candidates from another workspace,
	// and DB2 rehydrating them would leak their existence through the timing
	// even after refusing to return their content.
	filters := scopeFilters(request)

	candidates := p.index.Search(p.collection, request.Vector, int(request.TopK), filters)
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
		if err := p.index.Upsert(apply.Collection, apply.PointID, apply.Vector, apply.Labels); err != nil {
			// A dimension mismatch is the caller's error and will never
			// succeed on retry, so it is rejected rather than deferred.
			return db3.ProviderApplyOutcome{Result: db3.AppliedRejected}
		}
	case db3.ApplyDelete:
		p.index.Delete(apply.Collection, apply.PointID)
	case db3.ApplyTombstone:
		p.index.Tombstone(apply.Collection, apply.PointID)
	default:
		return db3.ProviderApplyOutcome{Result: db3.AppliedRejected}
	}

	p.applied[apply.OperationID] = true
	watermark := p.advanceLocked()
	return db3.ProviderApplyOutcome{
		Result:    db3.AppliedOK,
		Watermark: watermark,
		Lag:       p.lagLocked(apply.OperationID, watermark),
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
	p.index.mu.Lock()
	p.index.watermark = p.contiguous
	p.index.mu.Unlock()
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
		Capabilities: p.Capabilities(),
		Search:       p.Search,
		Apply:        p.Apply,
	})
}
