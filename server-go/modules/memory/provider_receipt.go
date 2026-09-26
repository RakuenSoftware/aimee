package memory

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// These plans are NOT durable receipts. The host must synchronously append both
// prepared and dispatch_admitted to its existing WORM owner before transport.
// An absent later observation never proves that an admitted attempt was unsent.
const providerReceiptDetailMax = 16000

type providerReceiptBinding struct {
	ReplayExpiresAt string `json:"replay_expires_at,omitempty"`
	DispatchOwner   string `json:"dispatch_owner,omitempty"`
	RendererVersion string `json:"renderer_version,omitempty"`
	PolicyVersion   string `json:"policy_version,omitempty"`
	AssemblyDigest  string `json:"assembly_sha256,omitempty"`

	TurnID               string          `json:"turn_id"`
	ProducerBuild        string          `json:"producer_build"`
	CallerLimitsDigest   string          `json:"caller_limits_sha256"`
	OperatorLimitsDigest string          `json:"operator_limits_sha256"`
	SchemaVersion        int             `json:"schema_version"`
	AttemptID            string          `json:"attempt_id"`
	ProducerID           string          `json:"producer_id"`
	RequestID            string          `json:"request_id"`
	RequestBinding       string          `json:"request_binding"`
	Workspace            string          `json:"workspace"`
	Project              string          `json:"project"`
	Retention            string          `json:"retention_mode"`
	SourceCoverage       string          `json:"source_coverage"`
	Sources              json.RawMessage `json:"sources"`
	SourcesDigest        string          `json:"sources_digest"`
	SourceCheckID        string          `json:"source_check_id"`
	Route                string          `json:"route"`
	Provider             string          `json:"provider"`
	Model                string          `json:"model"`
	PayloadDigest        string          `json:"payload_sha256"`
	PayloadBytes         string          `json:"payload_bytes"`
	CountProvenance      string          `json:"count_provenance"`
	TokenCount           *int64          `json:"token_count"`
}

type providerReceiptEvent struct {
	Projection             json.RawMessage         `json:"projection,omitempty"`
	ResponseRepresentation string                  `json:"response_representation,omitempty"`
	SchemaVersion          int                     `json:"schema_version"`
	Stage                  string                  `json:"stage"`
	AttemptID              string                  `json:"attempt_id"`
	At                     string                  `json:"at"`
	BindingDigest          string                  `json:"binding_sha256"`
	Binding                *providerReceiptBinding `json:"binding,omitempty"`
	HTTPStatus             int                     `json:"http_status,omitempty"`
	ResponseDigest         string                  `json:"response_sha256,omitempty"`
	ResponseBytes          string                  `json:"response_bytes,omitempty"`
	Reason                 string                  `json:"reason,omitempty"`
}

type providerReceiptEntry struct {
	binding, digest, attempt string
	prepared, admitted       string
	at                       string
	observation              string
	observationInput         string
	started                  string
	expires                  time.Time
	persistedAt              time.Time
}

func receiptDigestValid(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, c := range value {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func receiptByteCount(value string) bool {
	n, err := strconv.ParseUint(value, 10, 63)
	return err == nil && strconv.FormatUint(n, 10) == value
}
func receiptEventJSON(event providerReceiptEvent) (string, bool) {
	raw, err := json.Marshal(event)
	return string(raw), err == nil && len(raw) <= providerReceiptDetailMax
}

// Caller holds the source-release mutex. The single-use successful source check
// binds one concrete body/attempt. Retrying either transport or preparation needs
// a fresh owner check and receives a distinct attempt identity.
func (s *sourceReleaseState) receiptPlan(args commandArgs, entry *sourceReleaseEntry) ([]byte, bus.ModuleStatus) {
	coverage := "retained_versioned_inputs"
	check := ""
	if entry != nil {
		check = entry.admitted
		entry.admitted = ""
		if check == "" {
			return commandResult(commandError("unavailable", "provider receipt requires fresh source admission"))
		}
	} else {
		// Body receipts also cover host requests whose input channels do not yet
		// have source versions. Empty source metadata is an explicit gap, never
		// evidence of an empty memory input or a successful source check.
		coverage = "no_versioned_source_handle"
		entry = &sourceReleaseEntry{sources: json.RawMessage(`[]`),
			binding: releaseBinding(args), digest: releaseDigest([]typedProjectionRef{})}
	}
	digest, count := args.stringOr("payload_sha256", ""), args.stringOr("payload_bytes", "")
	route := args.stringOr("route", "")
	requestID, provider, model := args.stringOr("request_id", ""), args.stringOr("provider", ""), args.stringOr("model", "")
	turn, build := args.stringOr("turn_id", ""), args.stringOr("producer_build", "")
	callerLimits, operatorLimits := args.stringOr("caller_limits_sha256", ""), args.stringOr("operator_limits_sha256", "")
	if !receiptDigestValid(digest) || !receiptByteCount(count) ||
		(route != "openai_chat" && route != "openai_responses" && route != "anthropic_messages") ||
		len(requestID) > 256 || len(provider) > 1024 || len(model) > 1024 ||
		len(turn) > 128 || len(build) > 128 || !receiptDigestValid(callerLimits) || !receiptDigestValid(operatorLimits) {
		return commandResult(commandError("unavailable", "provider receipt binding unavailable"))
	}
	attempt, err := releaseToken()
	if err != nil {
		return commandResult(commandError("unavailable", "provider attempt identity unavailable"))
	}
	if s.receiptProducer == "" {
		s.receiptProducer, err = releaseToken()
		if err != nil {
			return commandResult(commandError("unavailable", "provider producer identity unavailable"))
		}
	}
	binding := providerReceiptBinding{SchemaVersion: 1, AttemptID: attempt, ProducerID: s.receiptProducer,
		TurnID: turn, ProducerBuild: build, CallerLimitsDigest: callerLimits, OperatorLimitsDigest: operatorLimits,
		RequestID: requestID, RequestBinding: entry.binding, Workspace: entry.workspace, Project: entry.project,
		Retention: "commitment_only", SourceCoverage: coverage, Sources: entry.sources,
		SourcesDigest: entry.digest, SourceCheckID: check, Route: route, Provider: provider, Model: model,
		PayloadDigest: digest, PayloadBytes: count, CountProvenance: "host_final_provider_bytes"}
	if owner := args.stringOr("dispatch_owner", ""); owner != "" {
		if !releaseTokenValid(owner) {
			return commandResult(commandError("unavailable", "dispatch ownership unavailable"))
		}
		binding.SchemaVersion = 2
		binding.DispatchOwner = owner
		binding.RendererVersion = "go-memory-projection-v1"
		binding.PolicyVersion = "source-fence-v1/task-recovery-v2"
		binding.AssemblyDigest = entry.assemblyDigest
		if binding.AssemblyDigest == "" {
			binding.AssemblyDigest = releaseDigest([]any{"unavailable_assembly", entry.digest})
		}
	}
	retention := args.stringOr("receipt_retention", "commitment_only")
	if retention != "commitment_only" && retention != "replayable" {
		return commandResult(commandError("unavailable", "unsupported receipt retention mode"))
	}
	if retention == "replayable" {
		n, _ := strconv.ParseUint(count, 10, 64)
		if binding.SchemaVersion != 2 || n > 49152 {
			return commandResult(commandError("unavailable", "replayable receipt requires owned dispatch and at most 49152 payload bytes"))
		}
		binding.Retention = retention
		binding.ReplayExpiresAt = time.Now().Add(time.Hour).UTC().Format(time.RFC3339Nano)
	}
	now := time.Now()
	at := now.UTC().Format(time.RFC3339Nano)
	bindingDigest := releaseDigest(binding)
	event := providerReceiptEvent{SchemaVersion: 1, Stage: "prepared", AttemptID: attempt, At: at, BindingDigest: bindingDigest, Binding: &binding}
	prepared, ok := receiptEventJSON(event)
	if !ok {
		return commandResult(commandError("unavailable", "provider receipt exceeds durable row capacity"))
	}
	event.Stage, event.Binding = "dispatch_admitted", nil
	admitted, ok := receiptEventJSON(event)
	if !ok || !s.receiptRoom(1, len(prepared)+len(admitted)) {
		return commandResult(commandError("unavailable", "provider receipt capacity unavailable"))
	}
	if s.receipts == nil {
		s.receipts = map[string]*providerReceiptEntry{}
	}
	s.receipts[attempt] = &providerReceiptEntry{binding: entry.binding, digest: bindingDigest, attempt: attempt,
		prepared: prepared, admitted: admitted, at: at, expires: now.Add(sourceReleaseTTL)}
	s.receiptBytes += len(prepared) + len(admitted)
	prelude := []map[string]string{}
	metadata := receiptMetadataWithHealth(entry)
	if len(metadata) > 0 {
		for _, stage := range []string{"retrieved", "assembled"} {
			detail, ok := receiptEventJSON(providerReceiptEvent{SchemaVersion: 1, Stage: stage, AttemptID: attempt, At: at, BindingDigest: bindingDigest, Projection: metadata, Reason: "assembly_observed_before_preparation"})
			if !ok {
				return commandResult(commandError("unavailable", "assembly receipt exceeds bound"))
			}
			prelude = append(prelude, map[string]string{"stage": stage, "detail": detail})
		}
	}
	var offer map[string]any
	if entry.assemblyDigest != "" {
		offer = s.explorationOfferForEntry(entry)
		if offer != nil {
			offer["producer_build"] = binding.ProducerBuild
			offer["route"], offer["provider"], offer["model"] = binding.Route, binding.Provider, binding.Model
			offer["limits_digest"] = releaseDigest([]string{binding.CallerLimitsDigest, binding.OperatorLimitsDigest, binding.RendererVersion, binding.PolicyVersion})
			offer["receipt_digest"] = bindingDigest
		}
	}
	return commandResult(map[string]any{"status": "ok", "durable": false, "requires_durable_acceptance": true,
		"exploration_offer": offer,
		"attempt_id":        attempt, "at": at, "assembly_events": prelude, "prepared_detail": prepared, "admitted_detail": admitted, "replay_store": binding.Retention == "replayable"})
}

// Host observations remain distinct from intent. A transport error, including a
// timeout after a possible send, cannot prove that the provider did no work.
func (s *sourceReleaseState) receiptObservation(args commandArgs) ([]byte, bus.ModuleStatus) {
	entry := s.receipts[args.stringOr("attempt_id", "")]
	if entry == nil || entry.binding != releaseBinding(args) {
		return commandResult(commandError("unavailable", "provider receipt observation unavailable"))
	}
	var status int
	if json.Unmarshal(args["http_status"], &status) != nil || (status != -1 && (status < 100 || status > 599)) {
		return commandResult(commandError("unavailable", "provider transport observation invalid"))
	}
	digest, count := args.stringOr("response_sha256", ""), args.stringOr("response_bytes", "")
	representation := "host_buffered_response_string" // pre-stream host protocol
	if _, present := args["response_representation"]; present {
		var ok bool
		representation, ok = args.stringValue("response_representation")
		if !ok {
			return commandResult(commandError("unavailable", "provider response representation invalid"))
		}
	}
	if !receiptDigestValid(digest) || !receiptByteCount(count) || (representation != "host_buffered_response_string" && representation != "provider_stream_bytes") {
		return commandResult(commandError("unavailable", "provider response commitment invalid"))
	}
	input := releaseDigest([]any{status, digest, count, representation})
	if entry.observation != "" {
		if input != entry.observationInput {
			return commandResult(commandError("unavailable", "provider observation conflicts with previous observation"))
		}
	} else {
		event := providerReceiptEvent{SchemaVersion: 1, Stage: "acknowledged", AttemptID: entry.attempt,
			At: time.Now().UTC().Format(time.RFC3339Nano), BindingDigest: entry.digest, HTTPStatus: status,
			ResponseDigest: digest, ResponseBytes: count, ResponseRepresentation: representation}
		if status == -1 {
			event.Stage, event.HTTPStatus, event.Reason = "outcome_unknown", 0, "transport_outcome_unresolved"
		}
		raw, ok := receiptEventJSON(event)
		if !ok || !s.receiptRoom(0, len(raw)) {
			return commandResult(commandError("unavailable", "provider observation capacity unavailable"))
		}
		entry.observation, entry.observationInput = raw, input
		s.receiptBytes += len(raw)
	}
	return commandResult(map[string]any{"status": "ok", "durable": false, "attempt_id": entry.attempt, "observation_detail": entry.observation})
}

// Completed observations remain replayable while cached. Under pressure only
// entries whose exact observation the trusted host reports durably appended can
// be reclaimed. Missing observations or failed writes never become evictable.
// Reclamation changes neither WORM retention nor the recorded transport outcome.
func (s *sourceReleaseState) receiptRoom(entries, bytes int) bool {
	for len(s.receipts)+entries > sourceReleaseMaxEntries || s.receiptBytes+bytes > sourceReleaseMaxBytes {
		oldest := ""
		var at time.Time
		for id, entry := range s.receipts {
			if entry == nil || entry.persistedAt.IsZero() {
				continue
			}
			if oldest == "" || entry.persistedAt.Before(at) || entry.persistedAt.Equal(at) && id < oldest {
				oldest, at = id, entry.persistedAt
			}
		}
		if oldest == "" {
			return false
		}
		entry := s.receipts[oldest]
		delete(s.receipts, oldest)
		s.receiptBytes -= len(entry.prepared) + len(entry.admitted) + len(entry.observation) + len(entry.started)
	}
	return true
}

func (s *sourceReleaseState) receiptStored(args commandArgs) ([]byte, bus.ModuleStatus) {
	entry := s.receipts[args.stringOr("attempt_id", "")]
	digest := args.stringOr("observation_sha256", "")
	if entry == nil || entry.binding != releaseBinding(args) || entry.observation == "" || !receiptDigestValid(digest) || digest != releaseDigest(json.RawMessage(entry.observation)) {
		return commandResult(commandError("unavailable", "durable observation confirmation unavailable"))
	}
	if entry.persistedAt.IsZero() {
		entry.persistedAt = time.Now()
	}
	return commandResult(map[string]any{"status": "ok", "cache_reclaimable": true, "durability_evidence": "trusted_host_append_confirmation"})
}

// Called only after the host's concrete request-write invocation returned.
// This is an observation of transport work, never proof of provider execution.
func (s *sourceReleaseState) receiptStarted(args commandArgs) ([]byte, bus.ModuleStatus) {
	e := s.receipts[args.stringOr("attempt_id", "")]
	if e == nil || e.binding != releaseBinding(args) {
		return commandResult(commandError("unavailable", "provider attempt unavailable"))
	}
	if e.started == "" {
		raw, ok := receiptEventJSON(providerReceiptEvent{SchemaVersion: 1, Stage: "dispatch_started", AttemptID: e.attempt, At: time.Now().UTC().Format(time.RFC3339Nano), BindingDigest: e.digest, Reason: "request_write_returned"})
		if !ok || !s.receiptRoom(0, len(raw)) {
			return commandResult(commandError("unavailable", "provider observation exceeds bound"))
		}
		e.started = raw
		s.receiptBytes += len(raw)
	}
	return commandResult(map[string]any{"status": "ok", "durable": false, "observation_detail": e.started})
}

// This operation is private to the authenticated host adapter. Caller-supplied
// receipts use verify_receipt and cannot assert ledger inclusion or ownership.
func inspectProviderReceipts(args commandArgs) ([]byte, bus.ModuleStatus) {
	type row struct {
		Sequence string `json:"sequence"`
		Action   string `json:"action"`
		Attempt  string `json:"attempt_id"`
		Detail   string `json:"detail"`
		Hash     string `json:"row_hash"`
	}
	var rows []row
	owner, request := args.stringOr("dispatch_owner", ""), args.stringOr("receipt_request_id", "")
	if !releaseTokenValid(owner) || request == "" || len(request) > 256 || !args.boolean("chain_intact") || len(args["ledger_events"]) > 256<<10 || json.Unmarshal(args["ledger_events"], &rows) != nil || len(rows) > 64 {
		return commandResult(commandError("unavailable", "complete verified receipt ledger unavailable"))
	}
	type attempt struct {
		PreparedSequence                         uint64
		Prepared                                 *providerReceiptEvent
		Stages                                   []string
		Projection                               json.RawMessage
		Admitted, Started, Acknowledged, Unknown bool
	}
	attempts := map[string]*attempt{}
	preludes := map[string][]providerReceiptEvent{}
	order := []string{}
	var sequence uint64
	for _, r := range rows {
		seq, err := strconv.ParseUint(r.Sequence, 10, 64)
		if err != nil || seq <= sequence || !receiptDigestValid(r.Hash) {
			return commandResult(commandError("unavailable", "invalid receipt ledger ordering"))
		}
		sequence = seq
		var event providerReceiptEvent
		if json.Unmarshal([]byte(r.Detail), &event) != nil || event.AttemptID != r.Attempt || r.Action != "memory.provider."+event.Stage {
			return commandResult(commandError("unavailable", "invalid receipt ledger event"))
		}
		if event.Stage == "retrieved" || event.Stage == "assembled" {
			if attempts[r.Attempt] != nil || len(preludes[r.Attempt]) >= 2 {
				return commandResult(commandError("unavailable", "invalid assembly stage order"))
			}
			preludes[r.Attempt] = append(preludes[r.Attempt], event)
			continue
		}
		a := attempts[r.Attempt]
		if event.Stage == "prepared" {
			p, ok := decodePreparedReceipt([]byte(r.Detail))
			if !ok || a != nil || p.Binding.RequestID != request || p.BindingDigest != releaseDigest(p.Binding) || p.Binding.SourcesDigest != releaseDigest(json.RawMessage(p.Binding.Sources)) {
				return commandResult(commandError("unavailable", "invalid prepared receipt binding"))
			}
			a = &attempt{Prepared: p, PreparedSequence: seq, Stages: []string{}}
			for _, prior := range preludes[r.Attempt] {
				if prior.BindingDigest != p.BindingDigest {
					return commandResult(commandError("unavailable", "assembly binding mismatch"))
				}
				a.Stages = append(a.Stages, prior.Stage)
				a.Projection = prior.Projection
			}
			delete(preludes, r.Attempt)
			attempts[r.Attempt] = a
			order = append(order, r.Attempt)
		}
		if a == nil || event.BindingDigest != a.Prepared.BindingDigest {
			return commandResult(commandError("unavailable", "receipt stage has no matching preparation"))
		}
		switch event.Stage {
		case "prepared":
		case "dispatch_admitted":
			a.Admitted = true
		case "dispatch_started":
			if !a.Admitted {
				return commandResult(commandError("unavailable", "transport observation lacks admission"))
			}
			a.Started = true
		case "acknowledged":
			if !a.Admitted || event.HTTPStatus < 100 || event.HTTPStatus > 599 {
				return commandResult(commandError("unavailable", "acknowledgement lacks admission"))
			}
			a.Acknowledged = true
		case "outcome_unknown":
			a.Unknown = true
		default:
			return commandResult(commandError("unavailable", "unsupported receipt stage"))
		}
		a.Stages = append(a.Stages, event.Stage)
	}
	if len(preludes) > 0 {
		return commandResult(commandError("unavailable", "incomplete preparation ledger"))
	}
	out := []map[string]any{}
	actions := []map[string]string{}
	var inputs map[string]string
	var deleted map[string]bool
	_ = json.Unmarshal(args["replay_inputs"], &inputs)
	_ = json.Unmarshal(args["replay_deleted"], &deleted)
	for _, id := range order {
		a := attempts[id]
		state := "outcome_unknown"
		ownership := "unresolved"
		if a.Prepared.Binding.DispatchOwner != "" {
			ownership = "current_dispatcher"
			if a.Prepared.Binding.DispatchOwner != owner {
				ownership = "prior_dispatcher_resolved"
			}
		}
		if a.Acknowledged {
			state = "acknowledged"
		} else if !a.Admitted && ownership == "prior_dispatcher_resolved" {
			state = "prepared_without_dispatch"
		} else if !a.Admitted && ownership == "current_dispatcher" {
			state = "prepared_dispatch_owner_active"
		}
		replay := "unavailable_commitment_only"
		payloadState := "requires_supplied_bytes"
		encoded := ""
		if a.Prepared.Binding.Retention == "replayable" {
			expiry, _ := time.Parse(time.RFC3339Nano, a.Prepared.Binding.ReplayExpiresAt)
			if args.boolean("forget_replay") || !time.Now().Before(expiry) {
				replay = "unavailable_expired"
				if args.boolean("forget_replay") {
					replay = "unavailable_removal_pending"
					if deleted[id] {
						replay = "unavailable_erased"
					}
				}
				if !deleted[id] {
					actions = append(actions, map[string]string{"attempt_id": id, "action": "delete"})
				}
			} else if value, present := inputs[id]; present {
				replay = "unavailable_inputs"
				payload, err := base64.StdEncoding.Strict().DecodeString(value)
				digest := sha256.Sum256(payload)
				if err == nil && value != "" && base64.StdEncoding.EncodeToString(payload) == value && len(payload) <= 49152 && hex.EncodeToString(digest[:]) == a.Prepared.Binding.PayloadDigest && strconv.Itoa(len(payload)) == a.Prepared.Binding.PayloadBytes {
					replay = "available_exact_payload"
					payloadState = "matched"
					if args.boolean("include_payload") {
						encoded = value
					}
				}
			} else {
				replay = "unavailable_inputs"
				actions = append(actions, map[string]string{"attempt_id": id, "action": "read"})
			}
		}
		out = append(out, map[string]any{"attempt_id": id, "prepared_sequence": strconv.FormatUint(a.PreparedSequence, 10), "state": state, "dispatch_ownership": ownership, "stages": a.Stages, "prepared_receipt": a.Prepared, "assembly": a.Projection,
			"evidence":       map[string]any{"schema_valid": true, "authenticated_producer": "local_host_ledger", "source_version_available": "not_checked", "payload_verifiable": payloadState, "decision_replayed": false, "chain_included": true, "externally_compared": "unavailable", "effect_confirmed": false},
			"retention_mode": a.Prepared.Binding.Retention, "replay": replay, "payload_base64": encoded, "local_acceptance": "durable", "checkpoint_state": args.stringOr("checkpoint_state", "unknown")})
	}
	return commandResult(map[string]any{"status": "ok", "request_id": request, "receipts": out, "complete": true, "replay_actions": actions})
}
