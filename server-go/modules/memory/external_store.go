package memory

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	contract "github.com/JBailes/aimee/server-go/memory"
	"github.com/JBailes/aimee/server-go/modules/audit"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"github.com/JBailes/aimee/server-go/modules/memory/cognee"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
)

// externalDataStore is a complete baseline store backed by an engine-owned
// compatibility catalog, not a postgresDataStore with a retrieval override.
type externalDataStore struct {
	ContractDataStore
	catalog     *backendstore.Store
	backend     contract.Backend
	placement   Placement
	auditAction func(context.Context, audit.Action) error
}

func configuredExternalStore(executor egress.Executor, placement Placement) (*externalDataStore, error) {
	factory, err := configuredMemoryBackend(executor)
	if err != nil {
		return nil, err
	}
	if factory == nil {
		return nil, contract.ErrUnsupported
	}
	home := os.Getenv("AIMEE_HOME")
	node, err := identity.Read(home)
	if err != nil {
		return nil, err
	}
	engine := strings.TrimSpace(os.Getenv("AIMEE_MEMORY_BACKEND"))
	base := os.Getenv("AIMEE_MEMORY_BACKEND_DIR")
	if base == "" {
		base = filepath.Join(home, "memory-backends")
	}
	base, err = filepath.Abs(base)
	if err != nil {
		return nil, err
	}
	digest := sha256.Sum256([]byte(node.ID + "/" + engine + "/" + string(placement)))
	digest[6] = (digest[6] & 0x0f) | 0x50
	digest[8] = (digest[8] & 0x3f) | 0x80
	collection := fmt.Sprintf("%x-%x-%x-%x-%x", digest[:4], digest[4:6], digest[6:8], digest[8:10], digest[10:16])
	catalog, err := backendstore.New(filepath.Join(base, engine, string(placement)), collection)
	if err != nil {
		return nil, err
	}
	backend, err := factory(catalog)
	if err != nil {
		return nil, err
	}
	return &externalDataStore{ContractDataStore: ContractDataStore{backend}, catalog: catalog, backend: backend, placement: placement}, nil
}
func (s *externalDataStore) execute(ctx context.Context, request DataRequest, scope Scope, options handlerOptions, explicit bool) (response DataResponse, status bus.ModuleStatus) {
	response = DataResponse{Records: []Record{}}
	status = bus.ModuleStatusOK
	actor := personalCaller(options.commandContext, request.Authority)
	epoch, epochErr := s.catalog.AdmissionEpoch(ctx, actor.principal)
	if epochErr != nil {
		return response, bus.ModuleStatusInternal
	}
	scopes := []Scope{scope}
	if s.placement == PlacementKB && !explicit {
		scopes = externalAudience(scope, request.Workspace)
	}
	search := func(query, kind, tier string, limit int) ([]contract.Record, error) {
		result := []contract.Record{}
		for _, admitted := range scopes {
			remaining := min(256, limit-len(result))
			if remaining <= 0 {
				break
			}
			var records []contract.Record
			var err error
			if query == "" {
				records, err = s.catalog.Search(ctx, admitted, query, kind, tier, remaining)
			} else {
				records, err = s.backend.Search(ctx, admitted, query, kind, tier, remaining)
			}
			if err != nil {
				return nil, err
			}
			result = append(result, records...)
		}
		return result, nil
	}
	fail := func(err error) (DataResponse, bus.ModuleStatus) {
		refusal := func(kind, message string, retry bool) (DataResponse, bus.ModuleStatus) {
			failure := &contract.Failure{Kind: kind, Message: message, Retryable: retry}
			raw, _ := json.Marshal(commandError(kind, message))
			return DataResponse{Records: []Record{}, Failure: failure, Payload: raw}, bus.ModuleStatusOK
		}
		if ctx.Err() != nil {
			return response, bus.ModuleStatusCancelled
		}
		if errors.Is(err, backendstore.ErrErasedAdmission) {
			return refusal("erased_admission", "memory write predates retained erasure controls", false)
		}
		if errors.Is(err, contract.ErrCapacity) {
			return refusal("capacity", "memory backend request exceeds an advertised limit", false)
		}
		if errors.Is(err, contract.ErrUnsupported) {
			return refusal("unsupported", "memory backend capability is unavailable", false)
		}
		if errors.Is(err, contract.ErrUnavailable) {
			return refusal("unavailable", "memory backend dependency or unresolved send guard unavailable", true)
		}
		if errors.Is(err, backendstore.ErrConflict) {
			code := MutationVersionConflict
			return DataResponse{Code: &code}, bus.ModuleStatusOK
		}
		if errors.Is(err, backendstore.ErrReplayUnavailable) {
			code := MutationReplayUnavailable
			return DataResponse{Code: &code}, bus.ModuleStatusOK
		}
		if errors.Is(err, backendstore.ErrIdempotency) {
			code := MutationIdempotencyConflict
			return DataResponse{Code: &code}, bus.ModuleStatusOK
		}
		if code := mutationRefusal(err); code != 0 {
			return DataResponse{Code: &code}, bus.ModuleStatusOK
		}
		if errors.Is(err, contract.ErrNotFound) {
			return response, bus.ModuleStatusOK
		}
		if errors.Is(err, contract.ErrClientRequest) {
			return response, bus.ModuleStatusInvalidRequest
		}
		if errors.Is(err, contract.ErrUnsupported) {
			return response, bus.ModuleStatusCapabilityAbsent
		}
		if ctx.Err() != nil {
			return response, bus.ModuleStatusCancelled
		}
		return response, bus.ModuleStatusInternal
	}
	convert := func(records []contract.Record) error {
		for _, r := range records {
			out, err := fromContractRecord(r)
			if err != nil {
				return err
			}
			response.Records = append(response.Records, out)
		}
		return nil
	}
	switch request.Operation {
	case "recall-bundle", "briefing-bundle", "compose-recall":
		var err error
		response.Payload, err = s.externalBundle(ctx, request, scopes, search)
		if err != nil {
			return fail(err)
		}
	case "personal-source-revalidate", "source-revalidate":
		private := request.Operation == "personal-source-revalidate"
		if private != (s.placement == PlacementServer) || !request.Revalidation.valid() {
			return response, bus.ModuleStatusInvalidRequest
		}
		if s.placement == PlacementKB && request.Revalidation.SendGuard != "" && !sourceSendGuardAllowed(options.commandContext) {
			return response, bus.ModuleStatusCapabilityAbsent
		}
		eligible, err := s.externalRevalidate(ctx, request.Revalidation, scopes)
		if err != nil {
			return fail(err)
		}
		response.Payload, _ = json.Marshal(sourceGuardResponse(request.Revalidation, eligible))
	case "get":
		if request.ID <= 0 {
			return response, bus.ModuleStatusInvalidRequest
		}
		if request.ReadPolicy != nil && request.ReadPolicy.Mode != "current" {
			return fail(contract.ErrUnsupported)
		}
		var r contract.Record
		var err error
		if request.AtVersion != nil {
			r, err = s.catalog.GetVersion(ctx, scope, request.ID, request.AtVersion)
		} else {
			for _, admitted := range scopes {
				r, err = s.backend.Get(ctx, admitted, request.ID)
				if !errors.Is(err, contract.ErrNotFound) {
					break
				}
			}
		}
		if err != nil {
			return fail(err)
		}
		if err = convert([]contract.Record{r}); err != nil {
			return fail(err)
		}
	case "search", "server-search", "visible-search", "adaptive-search", "recall", "briefing", "list":
		query := request.Query
		if request.Operation == "list" || request.Operation == "briefing" {
			query = ""
		}
		records, err := search(query, request.Kind, request.Tier, request.Limit)
		if err != nil {
			return fail(err)
		}
		if err = convert(records); err != nil {
			return fail(err)
		}
	case "backend-list":
		records, err := s.catalog.List(ctx, scope, request.AfterID, request.Limit)
		if err != nil {
			return fail(err)
		}
		if err = convert(records); err != nil {
			return fail(err)
		}
	case "store", "insert-epistemic", "supersede", "update-as", "delete", "delete-as":
		digestRaw, _ := json.Marshal(request)
		digest := sha256.Sum256(digestRaw)
		deleting := request.Operation == "delete" || request.Operation == "delete-as"
		replacing := request.Operation == "supersede" || request.Operation == "update-as"
		if (deleting || replacing) && request.ID <= 0 {
			return response, bus.ModuleStatusInvalidRequest
		}
		confidence := 1.0
		if request.Confidence != nil {
			confidence = *request.Confidence
		}
		if actor.authority != AuthorityUser {
			confidence = min(confidence, .8)
		}
		if request.Tier == "L5" {
			confidence = min(confidence, .5)
		}
		content, err := screenMemoryWrite(request.Key, request.Content)
		if err != nil {
			return fail(err)
		}
		r := contract.Record{ID: request.ID, Scope: scope, Tier: request.Tier, Kind: request.Kind, Key: request.Key, Content: content, Confidence: confidence}
		if !deleting && !replacing && (r.ID != 0 || r.Key == "" || r.Kind == "") {
			return response, bus.ModuleStatusInvalidRequest
		}
		if !deleting {
			if err = screenModelMemory(actor.authority, r.Key, r.Content, request.UseCases); err != nil {
				return fail(err)
			}
		}
		// A full-record update starts from its admitted revision. The atomic callback
		// below refuses a concurrent replacement rather than losing its metadata.
		if replacing {
			old, e := s.catalog.Get(ctx, scope, request.ID)
			if e != nil {
				return fail(e)
			}
			r = old
			r.Content = content
			r.Confidence = confidence
			if request.ExpectedVersion == nil {
				request.ExpectedVersion = old.Version
			}
		}
		category := "agent_message"
		if actor.authority == AuthorityUser {
			category = "user_stated"
		}
		epistemic := request.EpistemicKind
		if replacing && epistemic == "" {
			var prior map[string]any
			if json.Unmarshal(r.Authorship, &prior) != nil {
				return fail(contract.ErrUnavailable)
			}
			epistemic, _ = prior["epistemic_kind"].(string)
		}
		r.Authorship, _ = json.Marshal(map[string]any{"category": category, "principal": actor.principal, "transport": actor.transport, "session_id": request.SessionID, "epistemic_kind": epistemic, "erasure_epoch": strconv.FormatInt(epoch, 10)})
		key := ""
		if request.IdempotencyKey != "" {
			namespace, _ := json.Marshal([]any{actor.principal, actor.transport, scope.Type, scope.Value, epoch, request.IdempotencyKey})
			key = string(namespace)
		}
		admission := func(old *contract.Record) error {
			if old != nil {
				var authorship PersonalAuthorship
				if json.Unmarshal(old.Authorship, &authorship) != nil {
					return contract.ErrUnavailable
				}
				if old.Historical && actor.authority != AuthorityUser {
					return errMutationReviewRequired
				}
				var metadata map[string]any
				if json.Unmarshal(old.Authorship, &metadata) != nil {
					return contract.ErrUnavailable
				}
				epistemic := old.Kind
				if value, ok := metadata["epistemic_kind"].(string); ok && value != "" {
					epistemic = value
				}
				if e := admitMemoryReplacement(epistemic, authorship.Category, actor.authority); e != nil {
					return e
				}
			}
			if deleting {
				if derived, ok := s.backend.(contract.DerivedStore); ok {
					return derived.Forget(ctx, scope, request.ID)
				}
			}
			return nil
		}
		out, deleted, err := s.catalog.Mutate(ctx, backendstore.Mutation{Scope: scope, Record: r, Expected: request.ExpectedVersion, Delete: deleting, Key: key, Digest: hex.EncodeToString(digest[:]), Admit: admission})
		if err != nil {
			return fail(err)
		}
		response.Deleted = deleted
		if request.Operation == "update-as" {
			code := MutationOK
			response.Code = &code
			response.IDs = []int64{out.ID}
		}
		if !deleting {
			if err = convert([]contract.Record{out}); err != nil {
				return fail(err)
			}
		}
	case "assemble-context", "context-block", "context-ingress":
		records, err := search(request.Query, request.Kind, request.Tier, request.Limit)
		if err != nil {
			return fail(err)
		}
		budget := 8192
		if request.assemblyBytes != nil {
			budget = *request.assemblyBytes
		}
		var block strings.Builder
		for _, r := range records {
			line := r.Key + ": " + r.Content + "\n"
			if block.Len()+len(line) > budget {
				break
			}
			block.WriteString(line)
		}
		text := block.String()
		response.Block = &text
	case "backend-capabilities":
		capability := s.backend.Capabilities()
		capability.Version = 2
		capability.Profile = "aimee-conversation-v2"
		capability.CanonicalOwner = "adapter-catalog"
		capability.CandidateSelection = "whole-catalog lexical preselection; provider reranking"
		capability.RetrievalComplete = false
		capability.Limits = contract.Limits{MaxCandidates: 256, MaxQueryBytes: 16384, MaxBodyBytes: 1 << 20, MaxCatalogBytes: backendstore.MaxSnapshot}
		if capability.Name == "cognee" {
			capability.Limits.MaxCandidates = cognee.MaxCandidates
		}
		capability.Optional = []string{"graph", "extraction", "learning", "native-served-views"}
		capability.Operations = append(capability.Operations, "durable-records", "versions", "history", "backend-list", "backend-export", "backend-import", "erase-backend", "recall-bundle", "compose-recall", "briefing-bundle", "source-revalidate", "send-guards", "bounded-candidates", "post-erasure-admission")
		response.Payload, _ = json.Marshal(capability)
	case "backend-export":
		if !verifiedRetryCaller(options.commandContext) || !options.commandContext.UserAuthority {
			return response, bus.ModuleStatusCapabilityAbsent
		}
		raw, err := s.catalog.ExportScope(ctx, scope)
		if err != nil {
			return fail(err)
		}
		if len(raw) > maxDataBody/2 {
			return fail(contract.ErrCapacity)
		}
		response.Payload = raw
	case "backend-import":
		if !verifiedRetryCaller(options.commandContext) || !options.commandContext.UserAuthority {
			return response, bus.ModuleStatusCapabilityAbsent
		}
		// Reset prior derived state before publishing the imported source catalog.
		if reset, ok := s.backend.(contract.DerivedResetter); ok {
			if err := reset.ResetDerived(ctx); err != nil {
				return fail(err)
			}
		}
		if err := s.catalog.ImportScope(ctx, scope, []byte(request.Content)); err != nil {
			return fail(err)
		}
		response.Deleted = true
	default:
		return fail(contract.ErrUnsupported)
	}
	return
}
