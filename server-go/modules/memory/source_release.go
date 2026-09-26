package memory

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const sourceReleaseTTL = 15 * time.Minute
const sourceReleaseMaxBytes = 16 << 20
const sourceReleaseMaxEntries = 1024
const sourceReleaseHealthMaxBytes = 1 << 20

// Opaque process-local handles keep memory policy and source arrays out of the
// C host. Losing the process or expiring an entry refuses release. These are
// neither durable prepared receipts nor acknowledgements of provider dispatch.
type sourceReleaseState struct {
	healthTasks     map[string]*healthTaskTurns
	healthQueries   map[string]healthPendingQuery
	healthBytes     int
	mu              sync.Mutex
	entries         map[string]*sourceReleaseEntry
	bytes           int
	receipts        map[string]*providerReceiptEntry
	receiptBytes    int
	receiptProducer string
}
type sourceReleasePart struct {
	IndexGeneration   string `json:"index_generation,omitempty"`
	Native            bool   `json:"native"`
	Digest            string `json:"digest"`
	CoverageStatus    string `json:"coverage_status,omitempty"`
	RequirementDigest string `json:"requirement_digest,omitempty"`
}

type sourceReleaseEntry struct {
	guardedAdmission                             string
	guardedAdmissionAt                           time.Time
	assemblyParts                                []sourceReleasePart
	assemblyDigest                               string
	assemblyMetadata                             json.RawMessage
	healthMetadata                               json.RawMessage
	sources                                      json.RawMessage
	workspace, project, binding, digest, pending string
	admitted                                     string
	pendingLocalDigest, pendingSharedDigest      string
	pendingGuard                                 bool
	previous                                     string
	expires                                      time.Time
}

func releaseDigest(value any) string {
	raw, _ := json.Marshal(value)
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:])
}
func releaseToken() (string, error) {
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(token[:]), nil
}
func releaseTokenValid(token string) bool {
	if len(token) != 32 {
		return false
	}
	for _, c := range token {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
func releaseBinding(args commandArgs) string {
	return releaseDigest([]string{args.stringOr("request_id", ""), args.stringOr("principal", ""), args.stringOr("caller_subject", "")})
}
func (s *sourceReleaseState) expire(now time.Time) {
	for token, receipt := range s.receipts {
		if !now.Before(receipt.expires) {
			delete(s.receipts, token)
			s.receiptBytes -= len(receipt.prepared) + len(receipt.admitted) + len(receipt.observation) + len(receipt.started)
		}
	}
	for token, e := range s.entries {
		if !now.Before(e.expires) {
			delete(s.entries, token)
			s.bytes -= len(e.sources) + len(e.assemblyMetadata)
			s.healthBytes -= len(e.healthMetadata)
		}
	}
}

func (s *sourceReleaseState) prepare(args commandArgs, assembly map[string]any) (string, error) {
	refs := []typedProjectionRef{}
	for _, name := range []string{"facts_projection", "typed_projection", "memory_projection", "native_projection"} {
		if projection, ok := assembly[name].(map[string]any); ok {
			for _, ref := range projection["retained_items"].([]typedProjectionRef) {
				// Unversioned channels remain explicitly outside this source check.
				if ref.Source != nil {
					refs = append(refs, ref)
				}
			}
		}
	}
	previous := args.stringOr("source_release_ticket", "")
	_, replaceNative := assembly["native_projection"]
	appendNative, _ := assembly["append_native_sources"].(bool)
	replaceNative = replaceNative && !appendNative
	if len(refs) == 0 && !replaceNative {
		return previous, nil
	}
	token, err := releaseToken()
	if err != nil {
		return "", err
	}
	workspace, project := args.stringOr("workspace", ""), args.stringOr("project", "")
	if len(workspace) > 1024 || len(project) > 1024 {
		return "", errors.New("scope too large")
	}
	binding := releaseBinding(args)
	s.mu.Lock()
	defer s.mu.Unlock()
	now := time.Now()
	s.expire(now)
	var prior *sourceReleaseEntry
	if previous != "" {
		prior = s.entries[previous]
		if prior == nil || prior.binding != binding || prior.workspace != workspace || prior.project != project {
			return "", errors.New("previous source release unavailable")
		}
		var old []typedProjectionRef
		if json.Unmarshal(prior.sources, &old) != nil {
			return "", errors.New("invalid previous release")
		}
		// The native host replaces its complete system-context block on refresh.
		// Its proofs are replaced with that block; ingress text remains retained.
		if replaceNative {
			kept := old[:0]
			for _, ref := range old {
				if !strings.HasPrefix(ref.Channel, "native_") {
					kept = append(kept, ref)
				}
			}
			old = kept
		}
		refs = append(old, refs...)
	}
	unique := make([]typedProjectionRef, 0, len(refs))
	seen := map[string]bool{}
	for _, ref := range refs {
		key := releaseDigest(ref)
		if !seen[key] {
			seen[key] = true
			unique = append(unique, ref)
		}
	}
	if len(unique) == 0 {
		return "", nil
	}
	request := sourceRevalidation{SchemaVersion: 1, CheckID: token, Sources: unique}
	if !request.valid() {
		return "", errors.New("unavailable source contract")
	}
	raw, err := json.Marshal(unique)
	if err != nil {
		return "", err
	}
	_, nativePart := assembly["native_projection"]
	part := sourceReleasePart{Native: nativePart, Digest: healthIndependentAssemblyDigest(assembly)}
	if context, ok := assembly["indexed_context"].(map[string]any); ok && context["project"] == project {
		part.IndexGeneration, _ = context["generation"].(string)
	}
	if projection, ok := assembly["typed_projection"].(map[string]any); ok {
		if coverage, ok := projection["evidence_coverage"].(*evidenceCoverage); ok && coverage != nil {
			part.CoverageStatus = coverage.Status
			part.RequirementDigest = coverage.RequirementDigest
		}
	}
	if prior != nil && bytes.Equal(raw, prior.sources) && len(prior.assemblyParts) > 0 && prior.assemblyParts[len(prior.assemblyParts)-1] == part {
		return previous, nil
	}
	// The commitment covers every retained block, not just the last appended
	// native projection. Refresh replaces native parts while retaining ingress.
	parts := []sourceReleasePart{}
	if prior != nil {
		for _, old := range prior.assemblyParts {
			if !replaceNative || !old.Native {
				parts = append(parts, old)
			}
		}
	}
	if len(parts) >= 128 {
		return "", errors.New("source assembly history capacity")
	}
	parts = append(parts, part)
	assemblyDigest := releaseDigest(parts)
	metadataFields := map[string]any{"packing_dispositions": assembly["packing_dispositions"], "context_accounting": assembly["context_accounting"], "projection_commitment": assemblyDigest, "projection_parts": parts}
	metadata, _ := json.Marshal(metadataFields)
	healthFields := map[string]any{}
	if os.Getenv("AIMEE_MEMORY_HEALTH_ENABLED") == "1" {
		if capture, ok := assembly["health_context"].(*healthQueryContext); ok && capture != nil {
			healthFields["health_context"] = capture
		}
		if prior != nil {
			var previous struct {
				Context *healthQueryContext `json:"health_context"`
			}
			if json.Unmarshal(prior.healthMetadata, &previous) == nil && previous.Context != nil {
				healthFields["health_context"] = previous.Context
			}
		}
		var previousMetadata json.RawMessage
		if prior != nil {
			previousMetadata = prior.healthMetadata
		}
		if records := mergeHealthSelectionMetadata(previousMetadata, assembly, unique); len(records) > 0 {
			healthFields["health_records"] = records
		}
		if labels, ok := assembly["health_labels"].(*healthLabelsEnvelope); ok && labels != nil && labels.SourcesDigest == releaseDigest(unique) {
			healthFields["health_labels"] = labels
		}
	}
	var healthMetadata json.RawMessage
	if len(healthFields) > 0 {
		healthMetadata, _ = json.Marshal(healthFields)
		combined := make(map[string]any, len(metadataFields)+len(healthFields))
		for key, value := range metadataFields {
			combined[key] = value
		}
		for key, value := range healthFields {
			combined[key] = value
		}
		encoded, _ := json.Marshal(combined)
		if len(encoded) > 12000 || s.healthBytes+len(healthMetadata) > sourceReleaseHealthMaxBytes {
			healthMetadata = nil
		}
	}

	if len(metadata) > 12000 {
		metadata, _ = json.Marshal(map[string]any{"projection_commitment": assemblyDigest, "truncated": true, "reason": "assembly_metadata_limit"})
	}
	if len(s.entries) >= sourceReleaseMaxEntries || s.bytes+len(metadata)+len(raw) > sourceReleaseMaxBytes || len(raw) > maxDataBody/2 {
		return "", errors.New("source release capacity")
	}
	if s.entries == nil {
		s.entries = map[string]*sourceReleaseEntry{}
	}
	// Assembly is not integrity acceptance. Keep the old handle immutable until
	// the host either discards this candidate or uses it at the provider fence.
	s.entries[token] = &sourceReleaseEntry{assemblyParts: parts, assemblyDigest: assemblyDigest, assemblyMetadata: metadata, healthMetadata: healthMetadata, sources: raw, workspace: workspace, project: project, binding: binding, digest: releaseDigest(unique), previous: previous, expires: now.Add(sourceReleaseTTL)}
	s.bytes += len(raw) + len(metadata)
	s.healthBytes += len(healthMetadata)
	return token, nil
}

func (s *sourceReleaseState) drop(token, binding string, ancestors bool) {
	for token != "" {
		entry := s.entries[token]
		if entry == nil || entry.binding != binding {
			return
		}
		delete(s.entries, token)
		s.bytes -= len(entry.sources) + len(entry.assemblyMetadata)
		s.healthBytes -= len(entry.healthMetadata)
		if !ancestors {
			return
		}
		token = entry.previous
	}
}

func handleSourceRelease(s *sourceReleaseState, args commandArgs) ([]byte, bus.ModuleStatus) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.expire(time.Now())
	ticket := args.stringOr("source_release_ticket", "")
	entry := s.entries[ticket]
	operation := args.stringOr("operation", "")
	if operation == "exploration-owner-generation" {
		if s.receiptProducer == "" {
			return commandResult(commandError("unavailable", "memory producer unavailable"))
		}
		return commandResult(map[string]any{"status": "ok", "generation_only": true, "memory_owner": s.receiptProducer})
	}
	if operation == "health-turn-finish" {
		return s.healthTurnFinish(args)
	}
	if operation == "provider-receipt-started" {
		return s.receiptStarted(args)
	}
	if operation == "provider-receipt-observe" {
		return s.receiptObservation(args)
	}
	if operation == "provider-receipt-stored" {
		return s.receiptStored(args)
	}
	if operation == "provider-receipt-plan" && ticket == "" {
		if _, present := args["source_release_ticket"]; present {
			if _, ok := args.stringValue("source_release_ticket"); !ok {
				return commandResult(commandError("unavailable", "invalid source release handle"))
			}
		}
		return s.receiptPlan(args, nil)
	}
	if (operation == "source-release-finish" || operation == "source-release-discard") && (entry == nil || entry.binding == releaseBinding(args)) {
		s.drop(ticket, releaseBinding(args), operation == "source-release-finish")
		return commandResult(map[string]any{"status": "ok"})
	}
	if entry == nil || entry.binding != releaseBinding(args) {
		return commandResult(commandError("unavailable", "source release handle unavailable"))
	}
	if operation == "exploration-owner-observe" {
		return commandResult(map[string]any{"status": "ok", "memory_owner": s.receiptProducer,
			"plan_digest": entry.assemblyDigest, "source_versions_digest": releaseDigest(json.RawMessage(entry.sources))})
	}
	if operation == "provider-receipt-plan" {
		return s.receiptPlan(args, entry)
	}
	if operation == "source-release-plan" {
		entry.admitted = ""
		entry.guardedAdmission = ""
		entry.guardedAdmissionAt = time.Time{}
		// Reaching the fence confirms host integrity acceptance. The cumulative
		// source set supersedes its earlier handles; a rejected candidate never does.
		s.drop(entry.previous, entry.binding, true)
		entry.previous = ""
		check, err := releaseToken()
		if err != nil {
			return commandResult(commandError("unavailable", "source check unavailable"))
		}
		entry.pending = check
		var guarded bool
		if raw, present := args["send_guard"]; present && json.Unmarshal(raw, &guarded) != nil {
			return commandResult(commandError("invalid_argument", "invalid send guard"))
		}
		entry.pendingGuard = guarded
		var refs []typedProjectionRef
		if json.Unmarshal(entry.sources, &refs) != nil {
			return commandResult(commandError("unavailable", "source release data unavailable"))
		}
		local, shared := []typedProjectionRef{}, []typedProjectionRef{}
		for _, ref := range refs {
			if ref.Source.Kind == "user_memory_record" || ref.Source.Kind == "user_memory_collection" {
				local = append(local, ref)
			} else {
				shared = append(shared, ref)
			}
		}
		entry.pendingLocalDigest, entry.pendingSharedDigest = "", ""
		result := map[string]any{"status": "ok"}
		revalidation := func(refs []typedProjectionRef) sourceRevalidation {
			r := sourceRevalidation{SchemaVersion: 1, CheckID: check, Sources: refs}
			if guarded {
				r.SendGuard = "acquire"
			}
			return r
		}
		if len(local) > 0 {
			entry.pendingLocalDigest = releaseDigest(local)
			result["local_request"] = map[string]any{"operation": "personal-source-revalidate", "revalidation": revalidation(local)}
			if guarded {
				r := revalidation(local)
				r.SendGuard = "release"
				r.Sources = nil
				result["local_release_request"] = map[string]any{"operation": "personal-source-revalidate", "revalidation": r}
			}
		}
		if len(shared) > 0 {
			entry.pendingSharedDigest = releaseDigest(shared)
			result["request"] = map[string]any{"scope_context": true, "include_all": false, "workspace": entry.workspace, "project": entry.project, "revalidation": revalidation(shared)}
			if guarded {
				r := revalidation(shared)
				r.SendGuard = "release"
				r.Sources = nil
				result["release_request"] = map[string]any{"scope_context": true, "include_all": false, "workspace": entry.workspace, "project": entry.project, "revalidation": r}
			}
		}
		return commandResult(result)
	}
	entry.admitted = ""
	entry.guardedAdmission = ""
	entry.guardedAdmissionAt = time.Time{}
	check := entry.pending
	guarded := entry.pendingGuard
	entry.pendingGuard = false
	localDigest, sharedDigest := entry.pendingLocalDigest, entry.pendingSharedDigest
	entry.pending, entry.pendingLocalDigest, entry.pendingSharedDigest = "", "", "" // one answer set per attempt
	if check == "" || (localDigest == "" && sharedDigest == "") {
		return commandResult(commandError("unavailable", "source owner answer unavailable"))
	}
	eligible := true
	for _, part := range []struct{ field, digest string }{{"local_response", localDigest}, {"owner_response", sharedDigest}} {
		if part.digest == "" {
			continue
		}
		var reply struct {
			Status             string `json:"status"`
			Eligible           bool   `json:"eligible"`
			CheckID            string `json:"check_id"`
			Digest             string `json:"sources_digest"`
			SendGuard          string `json:"send_guard"`
			LeaseMillis        int    `json:"lease_ms"`
			GuardSchemaVersion int    `json:"guard_schema_version"`
		}
		if json.Unmarshal(args[part.field], &reply) != nil || reply.Status != "ok" || reply.CheckID != check || reply.Digest != part.digest || (guarded && reply.Eligible && (reply.SendGuard != "acquired" || reply.LeaseMillis != 5000 || reply.GuardSchemaVersion != 2)) {
			return commandResult(commandError("unavailable", "source owner answer unavailable"))
		}
		eligible = eligible && reply.Eligible
	}
	if !eligible {
		return commandResult(commandError("stale_context", "retained memory source changed or is no longer visible"))
	}

	entry.admitted = check
	if guarded {
		entry.guardedAdmission = check
		entry.guardedAdmissionAt = time.Now()
	}
	return commandResult(map[string]any{"status": "ok", "admitted": true, "boundary": "source_revalidation"})
}

// Exploration metadata is derived from the accepted final assembly, never from
// a hook-authored confidence value. It is an observation, not an access grant.
func (s *sourceReleaseState) explorationOffer(ticket string) map[string]any {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.explorationOfferForEntry(s.entries[ticket])
}

// Caller holds s.mu. Receipt preparation uses the same immutable plan entry.
func (s *sourceReleaseState) explorationOfferForEntry(e *sourceReleaseEntry) map[string]any {
	if e == nil {
		return nil
	}
	if s.receiptProducer == "" {
		owner, err := releaseToken()
		if err != nil {
			return nil
		}
		s.receiptProducer = owner
	}
	// Native recall refresh replaces its own block, not the retained ingress
	// requirements. Derive coverage from the same immutable parts committed by
	// the plan digest, rather than the last appended projection alone.
	complete := true
	class, requirement := "unclassified", "unavailable"
	requirements := []string{}
	generation := ""
	conflictingGenerations := false
	for _, part := range e.assemblyParts {
		if part.IndexGeneration != "" {
			if generation != "" && generation != part.IndexGeneration {
				conflictingGenerations = true
			}
			generation = part.IndexGeneration
		}
		if part.CoverageStatus == "" {
			continue
		}
		requirements = append(requirements, part.RequirementDigest)
		if part.CoverageStatus != "complete" || part.RequirementDigest == "" {
			complete = false
		}
	}
	if len(requirements) == 0 {
		complete = false
	} else {
		class, requirement = "typed_requirements", releaseDigest(requirements)
	}
	if generation == "" || conflictingGenerations {
		generation = "unavailable"
	}
	return map[string]any{"memory_owner": s.receiptProducer, "plan_digest": e.assemblyDigest, "source_versions_digest": releaseDigest(json.RawMessage(e.sources)), "query_class": class, "coverage_complete": complete, "confidence_provenance": "uncalibrated:" + requirement, "index_generation": generation, "expires": e.expires}
}
