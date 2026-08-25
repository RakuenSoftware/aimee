package postgres

import (
	"context"
	"errors"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db3"
)

// Connecting the router to a provisioned vector database.
//
// There is no discovery here and no second attachment. The grant directory is
// read once, at boot, by the module that will use the answer -- and it is the
// same directory the bus host read to decide what it would admit. If a grant
// was not made, this module serves every vector operation in-database for the
// rest of its life.

// ErrVectorBusConfig reports a bus attachment that cannot be used.
var ErrVectorBusConfig = errors.New("postgres: invalid vector provider attachment")

// ErrProviderRefused reports a provider that answered with a typed failure.
var ErrProviderRefused = errors.New("postgres: the vector provider refused the search")

// busSearcher adapts db3.SearchCaller to the router's ProviderSearcher.
//
// The adapter exists because the two disagree about what a provider's typed
// failure means, and that disagreement is deliberate. On the wire a failure is
// an ANSWER: the provider is saying it cannot serve this. To the router it must
// not be one, because acting on it as an answer returns no candidates, and no
// candidates is indistinguishable from a corpus with no matches.
type busSearcher struct {
	caller *db3.SearchCaller
}

// vectorSearchDeadline bounds how long a provider may take.
//
// Without one a search waits forever for a reply that may never come -- a
// provider that was killed mid-request leaves the caller blocked on a channel,
// and PostgreSQL, which could have answered immediately, is never asked. The
// fallback is always available, so waiting longer than this can only make the
// answer later, never better.
const vectorSearchDeadline = 2 * time.Second

func (s busSearcher) Search(ctx context.Context, principal uint32,
	request db3.SearchRequest) (db3.SearchReply, error) {
	// Bounded here, at the transport, rather than left to each caller: a caller
	// that forgot would hang, and the whole point of the fallback is that
	// nothing has to wait on the provider.
	bounded, cancel := context.WithTimeout(ctx, vectorSearchDeadline)
	defer cancel()
	reply, failure, err := s.caller.Search(bounded, request)
	if err != nil {
		return db3.SearchReply{}, err
	}
	if failure.Code != 0 {
		return db3.SearchReply{}, ErrProviderRefused
	}
	return reply, nil
}

// PolicyDir is where this deployment's module grants live.
//
// The same resolution the bus host performs, so the two cannot disagree about
// what was provisioned: AIMEE_MODULE_POLICY_DIR when set, otherwise
// <config>/modules.d/<daemon>.
func PolicyDir() string {
	if override := strings.TrimSpace(os.Getenv("AIMEE_MODULE_POLICY_DIR")); override != "" {
		return override
	}
	config := strings.TrimSpace(os.Getenv("AIMEE_CONFIG_DIR"))
	if config == "" {
		return ""
	}
	daemon := strings.TrimSpace(os.Getenv("AIMEE_DAEMON_NAME"))
	if daemon == "" {
		daemon = "server"
	}
	return config + "/modules.d/" + daemon
}

// VectorProvider is the provisioned vector database, or nothing.
type VectorProvider struct {
	// Principal is the provider's ref, or zero when none was provisioned.
	Principal uint32
	// Instance names it, for logs that have to say which one.
	Instance string
	// Collection is the one collection it serves. A search for any other is
	// answered in-database.
	Collection string
}

// ProvisionedVectorProvider reports what this deployment provisioned.
//
// Zero providers is the ordinary case and never an error. More than one is
// refused rather than chosen between: only one may serve the DB3 search kind --
// the bus binds a kind to exactly one slot -- so a second grant is a
// misconfiguration, and picking one silently would leave the operator with a
// provider that is installed, granted, and never used.
func ProvisionedVectorProvider(policyDir string) (VectorProvider, error) {
	grants := db3.ProviderGrants(policyDir)
	switch len(grants) {
	case 0:
		return VectorProvider{}, nil
	case 1:
		return VectorProvider{
			Principal:  grants[0].PrincipalRef,
			Instance:   grants[0].Instance,
			Collection: grants[0].Collection,
		}, nil
	default:
		names := make([]string, 0, len(grants))
		for _, grant := range grants {
			names = append(names, grant.Instance)
		}
		return VectorProvider{}, errors.New(
			"postgres: more than one vector provider is provisioned (" +
				strings.Join(names, ", ") + "); only one may serve the search")
	}
}

// VectorBus carries this module's vector searches on its own bus attachment.
type VectorBus struct {
	caller    *db3.SearchCaller
	publisher *db3.ApplyPublisher
	provider  VectorProvider

	// appliedMu guards the observer because replication installs it from the
	// goroutine that starts it while the module loop is already absorbing.
	appliedMu sync.RWMutex
	applied   func(uint32, db3.Applied)
}

// AttachVectorBus prepares the DB3 wire for a provisioned provider.
func AttachVectorBus(client *bus.Client, provider VectorProvider) (*VectorBus, error) {
	if client == nil {
		return nil, ErrVectorBusConfig
	}
	caller, err := db3.NewSearchCaller(client)
	if err != nil {
		return nil, err
	}
	// The write half. A provider that is searched and never written to answers
	// correctly and emptily forever, which reads as a corpus with no matches
	// rather than one nobody filled.
	publisher, err := db3.NewApplyPublisher(client)
	if err != nil {
		return nil, err
	}
	return &VectorBus{caller: caller, publisher: publisher, provider: provider}, nil
}

// Searcher is the transport to hand NewVectorRouter.
func (v *VectorBus) Searcher() ProviderSearcher { return busSearcher{caller: v.caller} }

// Provider is the provisioned provider this attachment serves.
func (v *VectorBus) Provider() VectorProvider {
	if v == nil {
		return VectorProvider{}
	}
	return v.provider
}

// ObserveApplied installs the sink for a provider's acknowledgements.
//
// Set by replication, which owns the ledger those acknowledgements settle. Until
// it is set, an Applied is dropped -- correct, because an operation the ledger
// never hears about is redelivered, and Apply is idempotent by operation id.
func (v *VectorBus) ObserveApplied(observer func(uint32, db3.Applied)) {
	if v == nil {
		return
	}
	v.appliedMu.Lock()
	v.applied = observer
	v.appliedMu.Unlock()
}

// Absorb hands one polled event to whichever half of the wire it belongs to.
func (v *VectorBus) Absorb(event bus.Event) {
	if v == nil {
		return
	}
	if event.Frame.EventKind == db3.EventApplied {
		v.absorbApplied(event)
		return
	}
	if v.caller != nil {
		v.caller.Absorb(event)
	}
}

// absorbApplied settles one acknowledgement against the outbox ledger.
//
// An Applied is the ONLY thing that deletes an outbox row -- publishing is an
// attempt, not evidence -- so this is the path that keeps the ledger from
// growing without bound and replaying every operation forever on lease expiry.
//
// Which makes the principal check load-bearing rather than defensive. Exactly
// one provider is granted; an Applied from any other principal is somebody else
// claiming an operation landed in a store this deployment does not read, and
// acting on it would delete the row that was the only record it had not.
func (v *VectorBus) absorbApplied(event bus.Event) {
	if event.Frame.PrincipalRef != v.provider.Principal {
		return
	}
	v.appliedMu.RLock()
	observer := v.applied
	v.appliedMu.RUnlock()
	if observer == nil {
		return
	}
	applied, err := db3.DecodeApplied(event.Payload)
	if err != nil || applied.Validate() != nil {
		// A malformed acknowledgement is not one. Dropping it leaves the
		// operation claimed until its lease expires and then redelivered, which
		// is the safe direction: Apply is idempotent by operation id.
		return
	}
	observer(event.Frame.PrincipalRef, applied)
}

// PublishApply ships one committed operation to the provisioned provider.
//
// The postgres module owns this because it owns the canonical rows: an
// operation is published only after it has committed here, so a provider can
// never hold a row PostgreSQL does not.
func (v *VectorBus) PublishApply(ctx context.Context, apply db3.Apply) error {
	if v == nil || v.publisher == nil {
		return ErrVectorBusConfig
	}
	return v.publisher.PublishApply(ctx, apply)
}

// Close releases the attachment.
func (v *VectorBus) Close() {
	if v == nil {
		return
	}
	if v.caller != nil {
		v.caller.Close()
	}
	if v.publisher != nil {
		v.publisher.Close()
	}
}
