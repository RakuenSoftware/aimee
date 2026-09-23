package memory

import (
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
	expires                  time.Time
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
		len(requestID) > 256 || len(provider) > 1024 || len(model) > 1024 || len(s.receipts) >= sourceReleaseMaxEntries ||
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
	if !ok || s.receiptBytes+len(prepared)+len(admitted) > sourceReleaseMaxBytes {
		return commandResult(commandError("unavailable", "provider receipt capacity unavailable"))
	}
	if s.receipts == nil {
		s.receipts = map[string]*providerReceiptEntry{}
	}
	s.receipts[attempt] = &providerReceiptEntry{binding: entry.binding, digest: bindingDigest, attempt: attempt,
		prepared: prepared, admitted: admitted, at: at, expires: now.Add(sourceReleaseTTL)}
	s.receiptBytes += len(prepared) + len(admitted)
	return commandResult(map[string]any{"status": "ok", "durable": false, "requires_durable_acceptance": true,
		"attempt_id": attempt, "at": at, "prepared_detail": prepared, "admitted_detail": admitted})
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
		if !ok || s.receiptBytes+len(raw) > sourceReleaseMaxBytes {
			return commandResult(commandError("unavailable", "provider observation capacity unavailable"))
		}
		entry.observation, entry.observationInput = raw, input
		s.receiptBytes += len(raw)
	}
	return commandResult(map[string]any{"status": "ok", "durable": false, "attempt_id": entry.attempt, "observation_detail": entry.observation})
}
