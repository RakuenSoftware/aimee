package vector

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	providerPollInterval  = 200 * time.Microsecond
	maxProviderAssemblies = 64
)

var ErrProviderConfig = errors.New("vector provider: invalid configuration")

type ProviderSearchFunc func(context.Context, SearchRequest) (SearchReply, SearchFailureCode)

type ProviderApplyOutcome struct {
	Result    AppliedResult
	Watermark uint64
	Lag       uint32
}

type ProviderApplyFunc func(context.Context, Apply) ProviderApplyOutcome

type ProviderConfig struct {
	Capabilities      Capabilities
	CapabilityUpdates <-chan Capabilities
	Search            ProviderSearchFunc
	Apply             ProviderApplyFunc
}

type providerWireClient interface {
	Poll() (bus.Event, bool, error)
	Publish(uint32, []byte) error
	ReplyFragment(uint32, uint64, []byte, bool) error
	HeartbeatNow()
	EpochChanged() bool
	InlineBudget() uint32
}

type providerAssembly struct {
	principal uint32
	body      []byte
}

type applyAssembly struct {
	total uint32
	next  uint32
	body  []byte
}

type applyAssemblyKey struct {
	principal uint32
	operation uint64
}

type providerSearchDone struct {
	correlation uint64
	request     SearchRequest
	reply       SearchReply
	failure     SearchFailureCode
}

type providerApplyDone struct {
	apply   Apply
	outcome ProviderApplyOutcome
}

// RunProvider serves one external DB3 implementation on an authenticated bus
// attachment. It never opens a socket or reaches DB2 directly.
func RunProvider(ctx context.Context, client *bus.Client, config ProviderConfig) error {
	if client == nil {
		return ErrProviderConfig
	}
	return runProvider(ctx, client, config)
}

func validateProviderConfig(ctx context.Context, client providerWireClient, config ProviderConfig) error {
	if ctx == nil || client == nil || client.InlineBudget() < capabilitiesHeader ||
		validateProviderCapabilities(config.Capabilities, config.Search, config.Apply) != nil {
		return ErrProviderConfig
	}
	return nil
}

func validateProviderCapabilities(capabilities Capabilities, search ProviderSearchFunc,
	apply ProviderApplyFunc) error {
	if capabilities.Validate() != nil {
		return ErrProviderConfig
	}
	if capabilities.Operations&OperationSearch != 0 && search == nil {
		return ErrProviderConfig
	}
	if capabilities.Operations&OperationApply != 0 && apply == nil {
		return ErrProviderConfig
	}
	return nil
}

func runProvider(ctx context.Context, client providerWireClient, config ProviderConfig) error {
	if err := validateProviderConfig(ctx, client, config); err != nil {
		return err
	}
	runCtx, stop := context.WithCancel(ctx)
	// Not discarded. A provider whose capabilities cannot be encoded announces
	// an empty frame, is never selected by the route, and reports nothing about
	// why -- which reads exactly like a provider nobody configured.
	capabilities, err := EncodeCapabilities(config.Capabilities)
	if err != nil {
		stop()
		return err
	}
	if err := providerPublish(runCtx, client, EventCapabilities, capabilities); err != nil {
		stop()
		return err
	}

	searches := make(map[uint64]providerAssembly)
	cancels := make(map[uint64]context.CancelFunc)
	applyChunks := make(map[applyAssemblyKey]applyAssembly)
	searchDone := make(chan providerSearchDone, maxProviderAssemblies)
	applyWork := make(chan Apply, maxProviderAssemblies)
	applyDone := make(chan providerApplyDone, maxProviderAssemblies)
	updates := config.CapabilityUpdates
	currentCapabilities := config.Capabilities
	var workers sync.WaitGroup
	workers.Add(1)
	go func() {
		defer workers.Done()
		for {
			var apply Apply
			select {
			case <-runCtx.Done():
				return
			case apply = <-applyWork:
			}
			done := providerApplyDone{apply: apply, outcome: config.Apply(runCtx, apply)}
			select {
			case applyDone <- done:
			case <-runCtx.Done():
				return
			}
		}
	}()
	defer func() {
		stop()
		for _, cancel := range cancels {
			cancel()
		}
		workers.Wait()
	}()

	idle := providerPollInterval
	for {
		select {
		case <-runCtx.Done():
			return nil
		case update, ok := <-updates:
			if !ok {
				updates = nil
				continue
			}
			if validateProviderCapabilities(update, config.Search, config.Apply) != nil {
				return ErrProviderConfig
			}
			wire, encodeErr := EncodeCapabilities(update)
			if encodeErr != nil {
				// The update was validated just above, so this cannot fail
				// today. Publishing an empty capabilities frame would tell every
				// caller this provider serves nothing, which is a much larger
				// claim than "an update could not be encoded".
				return encodeErr
			}
			if err := providerPublish(runCtx, client, EventCapabilities, wire); err != nil {
				return err
			}
			currentCapabilities = update
		case done := <-searchDone:
			if cancel := cancels[done.correlation]; cancel != nil {
				cancel()
				delete(cancels, done.correlation)
			}
			var (
				wire      []byte
				encodeErr error
			)
			if done.failure > SearchFailureInternal {
				done.failure = SearchFailureInternal
			}
			switch {
			case done.failure != 0:
				wire, encodeErr = EncodeSearchFailure(
					SearchFailure{RequestID: done.request.RequestID, Code: done.failure})
			case ValidateSearchReply(done.request, done.reply) != nil:
				wire, encodeErr = EncodeSearchFailure(
					SearchFailure{RequestID: done.request.RequestID, Code: SearchFailureInternal})
			default:
				wire, encodeErr = EncodeSearchReply(done.reply)
				if encodeErr != nil {
					// A reply that cannot be encoded is a reply that cannot be
					// sent as itself, which is the case immediately above. Same
					// answer, rather than an empty payload where a reply belongs.
					wire, encodeErr = EncodeSearchFailure(
						SearchFailure{RequestID: done.request.RequestID, Code: SearchFailureInternal})
				}
			}
			if encodeErr != nil {
				// Attempted once, never through the path that exists because
				// encoding failed. If the refusal itself cannot be encoded there
				// is nothing truthful left to send, and a provider that cannot
				// say anything should not go on answering.
				return encodeErr
			}
			if err := providerReply(runCtx, client, EventSearch, done.correlation, wire); err != nil {
				return err
			}
		case done := <-applyDone:
			applied := Applied{
				OperationID: done.apply.OperationID, Generation: done.apply.Generation,
				Watermark: done.outcome.Watermark, Result: done.outcome.Result, Lag: done.outcome.Lag,
			}
			if applied.Validate() != nil {
				applied = Applied{OperationID: done.apply.OperationID, Generation: done.apply.Generation,
					Result: AppliedInternal}
			}
			wire, encodeErr := EncodeApplied(applied)
			if encodeErr != nil {
				// Applied already falls back to AppliedInternal when it does not
				// validate, so this is the same decision one step further on.
				// An empty acknowledgement would leave the writer unable to tell
				// whether its generation landed.
				return encodeErr
			}
			if err := providerPublish(runCtx, client, EventApplied, wire); err != nil {
				return err
			}
		default:
		}

		if client.EpochChanged() {
			return bus.ErrEpoch
		}
		client.HeartbeatNow()
		event, ok, err := client.Poll()
		if err != nil {
			return err
		}
		if !ok {
			time.Sleep(idle)
			if idle < 10*time.Millisecond {
				idle *= 2
			}
			continue
		}
		idle = providerPollInterval
		switch event.Frame.EventKind {
		case bus.KindOverflow:
			clear(applyChunks)
		case EventSearch:
			if event.Frame.HdrFlags&bus.FCancel != 0 {
				if cancel := cancels[event.Frame.CorrelationID]; cancel != nil {
					cancel()
					delete(cancels, event.Frame.CorrelationID)
				}
				delete(searches, event.Frame.CorrelationID)
				continue
			}
			if event.Frame.HdrFlags&bus.FRequest == 0 || config.Search == nil {
				continue
			}
			handleProviderSearch(runCtx, event, currentCapabilities, config.Search, searches, cancels,
				searchDone)
		case EventApply:
			if event.Frame.HdrFlags&bus.FNotification == 0 || config.Apply == nil {
				continue
			}
			if apply, complete := decodeProviderApply(
				event.Frame.PrincipalRef, event.Payload, applyChunks,
			); complete {
				select {
				case applyWork <- apply:
				case <-runCtx.Done():
					return nil
				}
			}
		}
	}
}

func handleProviderSearch(parent context.Context, event bus.Event, capabilities Capabilities,
	search ProviderSearchFunc, assemblies map[uint64]providerAssembly,
	cancels map[uint64]context.CancelFunc, done chan<- providerSearchDone) {
	id := event.Frame.CorrelationID
	assembly, exists := assemblies[id]
	if (assembly.principal != 0 && assembly.principal != event.Frame.PrincipalRef) ||
		uint64(len(assembly.body))+uint64(len(event.Payload)) > uint64(bus.MaxPayload) {
		delete(assemblies, id)
		return
	}
	assembly.principal = event.Frame.PrincipalRef
	assembly.body = append(assembly.body, event.Payload...)
	if event.Frame.HdrFlags&bus.FMore != 0 {
		if exists || len(assemblies) < maxProviderAssemblies {
			assemblies[id] = assembly
		}
		return
	}
	delete(assemblies, id)
	request, err := DecodeSearchRequest(assembly.body)
	if err != nil {
		if len(assembly.body) >= 16 {
			requestID := uint64(0)
			for i := 0; i < 8; i++ {
				requestID |= uint64(assembly.body[8+i]) << (8 * i)
			}
			if requestID != 0 {
				done <- providerSearchDone{correlation: id, request: SearchRequest{RequestID: requestID},
					failure: SearchFailureInvalidRequest}
			}
		}
		return
	}
	if !capabilities.Ready || capabilities.Generation != request.RequiredGeneration ||
		capabilities.Operations&OperationSearch == 0 || capabilities.Metrics&MetricCosine == 0 ||
		capabilities.Filters&FilterExact == 0 ||
		uint32(len(request.Vector)) > capabilities.MaxDimension || request.TopK > capabilities.MaxTopK {
		done <- providerSearchDone{correlation: id, request: request,
			failure: SearchFailureUnavailable}
		return
	}
	if len(cancels) >= maxProviderAssemblies {
		done <- providerSearchDone{correlation: id, request: request,
			failure: SearchFailureRetryable}
		return
	}
	ctx, cancel := context.WithCancel(parent)
	cancels[id] = cancel
	go func() {
		reply, failure := search(ctx, request)
		if ctx.Err() != nil {
			return
		}
		select {
		case done <- providerSearchDone{
			correlation: id, request: request, reply: reply, failure: failure,
		}:
		case <-ctx.Done():
		}
	}()
}

func decodeProviderApply(principal uint32, payload []byte,
	assemblies map[applyAssemblyKey]applyAssembly) (Apply, bool) {
	if apply, err := DecodeApply(payload); err == nil {
		delete(assemblies, applyAssemblyKey{principal: principal, operation: apply.OperationID})
		return apply, true
	}
	chunk, err := DecodeApplyChunk(payload)
	if err != nil {
		return Apply{}, false
	}
	key := applyAssemblyKey{principal: principal, operation: chunk.OperationID}
	assembly, exists := assemblies[key]
	if chunk.Offset == 0 {
		if !exists && len(assemblies) >= maxProviderAssemblies {
			return Apply{}, false
		}
		assembly = applyAssembly{total: chunk.Total, body: make([]byte, 0, chunk.Total)}
		exists = true
	}
	if !exists || assembly.total != chunk.Total || assembly.next != chunk.Offset {
		delete(assemblies, key)
		return Apply{}, false
	}
	assembly.body = append(assembly.body, chunk.Data...)
	assembly.next += uint32(len(chunk.Data))
	if assembly.next < assembly.total {
		assemblies[key] = assembly
		return Apply{}, false
	}
	delete(assemblies, key)
	apply, err := DecodeApply(assembly.body)
	if err != nil || apply.OperationID != chunk.OperationID {
		return Apply{}, false
	}
	return apply, true
}

func providerPublish(ctx context.Context, client providerWireClient, kind uint32, payload []byte) error {
	for {
		err := client.Publish(kind, payload)
		if !errors.Is(err, bus.ErrWouldBlock) {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(providerPollInterval):
		}
	}
}

func providerReply(ctx context.Context, client providerWireClient, kind uint32,
	correlation uint64, payload []byte) error {
	budget := int(client.InlineBudget())
	if budget > int(bus.MaxPayload) {
		budget = int(bus.MaxPayload)
	}
	if budget <= 0 {
		return ErrProviderConfig
	}
	first := true
	for offset := 0; first || offset < len(payload); {
		first = false
		part := len(payload) - offset
		if part > budget {
			part = budget
		}
		more := offset+part < len(payload)
		for {
			err := client.ReplyFragment(kind, correlation, payload[offset:offset+part], more)
			if !errors.Is(err, bus.ErrWouldBlock) {
				if err != nil {
					return err
				}
				break
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(providerPollInterval):
			}
		}
		offset += part
	}
	return nil
}
