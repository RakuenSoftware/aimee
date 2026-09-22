package memory

import (
	"context"
	cryptorand "crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"math/rand/v2"
	"strings"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

const (
	memoryFactMaxAttempts = 3
	memoryFactMaxTriples  = 16
)

// FactActor is authority captured by the verified memory write boundary.
type FactActor struct {
	Principal         string `json:"principal"`
	TransportIdentity string `json:"transport_identity"`
	Role              string `json:"role"`
	Rank              int    `json:"rank"`
	Authenticated     int    `json:"authenticated"`
}

type FactEvidence struct {
	SourceKind     string `json:"source_kind"`
	SourceID       string `json:"source_id"`
	SourceSpan     string `json:"source_span"`
	EvidenceHash   string `json:"evidence_hash"`
	ActorPrincipal string `json:"actor_principal"`
	ObservedAt     string `json:"observed_at"`
	IngestRunID    string `json:"ingest_run_id"`
	Stance         string `json:"stance"`
}

// FactCandidate carries a grounded extraction into the Go transactional writer.
type FactCandidate struct {
	Subject       string       `json:"subject"`
	Relation      string       `json:"relation"`
	Object        string       `json:"object"`
	SubjectKind   NodeKind     `json:"subject_kind"`
	ObjectKind    NodeKind     `json:"object_kind"`
	Actor         FactActor    `json:"actor"`
	Evidence      FactEvidence `json:"evidence"`
	AssertionKind string       `json:"assertion_kind"`
	ValidFrom     string       `json:"valid_from,omitempty"`
	ValidUntil    string       `json:"valid_until,omitempty"`
}

type MemoryFactWork struct {
	Generation   int64  `json:"generation"`
	SourceHash   string `json:"source_hash"`
	LeaseToken   string `json:"lease_token"`
	JobID        int64  `json:"job_id"`
	MemoryID     int64  `json:"memory_id"`
	Attempts     int    `json:"attempts"`
	Content      string `json:"content"`
	SystemPrompt string `json:"system_prompt"`
}

type memoryFactRaw struct {
	Subject     string      `json:"subject"`
	Relation    string      `json:"relation"`
	Object      string      `json:"object"`
	Confidence  float64     `json:"confidence"`
	SourceStart json.Number `json:"source_start"`
	SourceEnd   json.Number `json:"source_end"`
}

type memoryFactEnvelope struct {
	Facts []memoryFactRaw `json:"facts"`
}

var relationAliases = map[string]string{
	"has_ip": "device_has_ip", "ip": "device_has_ip", "ip_address": "device_has_ip",
	"hostname": "has_hostname", "has_host": "has_hostname", "host_name": "has_hostname",
	"works_at": "works_for", "employed_by": "works_for", "employer": "works_for",
	"belongs_to": "member_of", "aka": "also_known_as", "alias": "also_known_as",
	"also_called": "also_known_as", "married_to": "spouse", "wife": "spouse",
	"husband": "spouse", "daughter": "child_of", "son": "child_of",
	"mother": "parent_of", "father": "parent_of", "mother_of": "parent_of",
	"father_of": "parent_of", "son_of": "child_of", "daughter_of": "child_of",
	"resides_in": "lives_in", "birthplace": "born_in", "governed_by": "linked_policy",
	"replaces": "supersedes",
}

func canonicalRelation(value string) string {
	normalized := normalizeRelType(value)
	if alias := relationAliases[normalized]; alias != "" {
		return alias
	}
	return normalized
}

func memoryFactSubjectKind(subject string) NodeKind {
	if strings.EqualFold(subject, "user") || strings.EqualFold(subject, "i") {
		return NodePerson
	}
	if subject != "" && subject[0] >= 'A' && subject[0] <= 'Z' {
		return NodePerson
	}
	return NodeOther
}

func memoryFactKinds(relation string, subjectKind, objectKind NodeKind) (NodeKind, NodeKind) {
	if def := seedLookup(relation); def != nil {
		if !kindAllowed(def.HeadKinds, subjectKind) && len(def.HeadKinds) > 0 {
			subjectKind = def.HeadKinds[0]
		}
		if !kindAllowed(def.TailKinds, objectKind) && len(def.TailKinds) > 0 {
			objectKind = def.TailKinds[0]
		}
	}
	return subjectKind, objectKind
}

func memoryFactPrompt() string {
	relations := make([]string, 0, len(seedOntology))
	for _, def := range seedOntology {
		relations = append(relations, def.RelType)
	}
	return "You extract durable facts from a single remembered note. Return ONLY a JSON object: " +
		`{"facts":[{"subject":"","relation":"","object":"","confidence":0.0,"source_start":0,"source_end":1}]}. ` +
		"source_start and source_end are exact zero-based UTF-8 byte offsets into the note, with source_end exclusive, " +
		"covering the smallest passage that directly supports that fact. Every fact is a stable subject-relation-object " +
		"triple grounded strictly in the note. For relation, choose the single nearest fit from these canonical predicates " +
		"when one reasonably applies: " + strings.Join(relations, ", ") + ". If NONE fits, emit a concise snake_case " +
		"predicate of your own (e.g. drives, founded, mentors) - NEVER a generic catch-all such as other/unknown/misc. " +
		"subject is the entity the fact is about (use user for the note's author when it is first-person). confidence is " +
		"0..1. Extract only durable, generalizable facts; skip transient state, feelings, plans, and one-off events. If the " +
		"note RETRACTS or DENIES something (no longer, did not, never, is not, has left, was removed), do NOT emit the " +
		"negated fact. Omit any fact whose exact supporting span cannot be identified. If the note asserts no durable fact, " +
		`return exactly {"facts":[]}. No prose, no markdown.`
}

func normalizeFactText(value string, capBytes int) string {
	if capBytes <= 0 {
		return ""
	}
	out := make([]byte, 0, len(value))
	space := true
	for i := 0; i < len(value) && len(out) < capBytes-1; i++ {
		c := value[i]
		if isAlnum(c) || c == '.' || c == ':' {
			out = append(out, toLower(c))
			space = false
		} else if !space {
			out = append(out, ' ')
			space = true
		}
	}
	return strings.TrimSuffix(string(out), " ")
}

func factGrounded(value, normalizedNote string) bool {
	normalized := normalizeFactText(value, 512)
	if normalized == "" || normalized == "user" || normalized == "i" || normalized == "me" {
		return true
	}
	if strings.Contains(normalizedNote, normalized) {
		return true
	}
	words, hits := 0, 0
	for _, word := range strings.Fields(normalized) {
		if len(word) <= 2 {
			continue
		}
		words++
		if strings.Contains(normalizedNote, word) {
			hits++
		}
	}
	return words > 0 && hits*2 >= words
}

func exactJSONInteger(value json.Number) (int64, bool) {
	f, err := value.Float64()
	if err != nil || math.IsNaN(f) || math.IsInf(f, 0) || math.Trunc(f) != f || f < 0 || f > math.MaxInt64 {
		return 0, false
	}
	return int64(f), true
}

func memoryFactEvidence(content string, start, end int64, actor FactActor, observedAt string, memoryID, jobID int64) FactEvidence {
	digest := sha256.Sum256([]byte(content[start:end]))
	return FactEvidence{
		SourceKind: "memory", SourceID: fmt.Sprintf("memory:%d", memoryID),
		SourceSpan: fmt.Sprintf("bytes:%d-%d", start, end), EvidenceHash: hex.EncodeToString(digest[:]),
		ActorPrincipal: actor.Principal, ObservedAt: observedAt,
		IngestRunID: fmt.Sprintf("memory-facts:%d", jobID), Stance: "supports",
	}
}

func modelFactActor() FactActor {
	return FactActor{Principal: "system:model-inference", TransportIdentity: "internal", Role: "model", Rank: 10}
}

func validCapturedFactActor(actor FactActor) bool {
	return actor.Principal != "" && actor.Role != "" &&
		(actor.Rank == 10 || actor.Rank == 20 || actor.Rank == 30)
}

func memoryFactRetryBase(attempts int) time.Duration {
	if attempts < 1 {
		attempts = 1
	}
	delay := 30 * time.Second
	for step := 1; step < attempts && delay < time.Hour; step++ {
		delay *= 2
	}
	if delay > time.Hour {
		return time.Hour
	}
	return delay
}

func memoryFactRetryDelay(attempts int) time.Duration {
	base := memoryFactRetryBase(attempts)
	span := base / 10
	if span <= 0 {
		return base
	}
	// This is load-spreading jitter, not a security decision. Match the legacy
	// curator retry window so a failed batch does not re-enter in lockstep.
	return base - span + time.Duration(rand.Int64N(int64(2*span)+1))
}

func memoryFactProviderUnavailable(reason string) bool {
	if reason == "" {
		return false
	}
	for _, marker := range []string{
		"provider HTTP 503", "provider HTTP 429", "provider HTTP -1",
		"HTTP 503 from", "HTTP 429 from",
		`"code": "provider_unavailable"`, `"code":"provider_unavailable"`,
		"upstream circuit is open", "no synthesis endpoint configured",
	} {
		if strings.Contains(reason, marker) {
			return true
		}
	}
	return strings.Contains(reason, "request to ") && strings.Contains(reason, "timed out")
}

func patternFactCandidates(content, observedAt string, memoryID, jobID int64, actor FactActor) []FactCandidate {
	triples := ExtractPatterns(content, memoryFactMaxTriples)
	evidence := memoryFactEvidence(content, 0, int64(len(content)), actor, observedAt, memoryID, jobID)
	out := make([]FactCandidate, 0, len(triples))
	for _, triple := range triples {
		relation := canonicalRelation(triple.RelType)
		subjectKind, objectKind := memoryFactKinds(relation, triple.SubjectKind, triple.ObjectKind)
		out = append(out, FactCandidate{Subject: triple.Subject, Relation: relation, Object: triple.Object,
			SubjectKind: subjectKind, ObjectKind: objectKind, Actor: actor, Evidence: evidence,
			AssertionKind: "world_fact", ValidFrom: observedAt})
	}
	return out
}

func parseModelFactCandidates(response, content, observedAt string, memoryID, jobID int64, sourceActor FactActor) ([]FactCandidate, error) {
	trimmed := strings.TrimSpace(response)
	if trimmed == "[]" {
		return nil, nil
	}
	start, end := strings.IndexByte(response, '{'), strings.LastIndexByte(response, '}')
	if start < 0 || end < start {
		return nil, nil
	}
	decoder := json.NewDecoder(strings.NewReader(response[start : end+1]))
	decoder.UseNumber()
	var envelope memoryFactEnvelope
	if err := decoder.Decode(&envelope); err != nil {
		return nil, nil
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return nil, nil
	}
	normalizedNote := normalizeFactText(content, 4096)
	modelActor := modelFactActor()
	out := make([]FactCandidate, 0, len(envelope.Facts))
	for _, fact := range envelope.Facts {
		if fact.Subject == "" || fact.Relation == "" || fact.Object == "" ||
			!factGrounded(fact.Subject, normalizedNote) || !factGrounded(fact.Object, normalizedNote) {
			continue
		}
		startOffset, startOK := exactJSONInteger(fact.SourceStart)
		endOffset, endOK := exactJSONInteger(fact.SourceEnd)
		if !startOK || !endOK || endOffset <= startOffset || endOffset > int64(len(content)) {
			continue
		}
		relation := canonicalRelation(fact.Relation)
		subjectKind, objectKind := memoryFactKinds(relation, memoryFactSubjectKind(fact.Subject), NodeOther)
		out = append(out, FactCandidate{Subject: fact.Subject, Relation: relation, Object: fact.Object,
			SubjectKind: subjectKind, ObjectKind: objectKind, Actor: modelActor,
			Evidence:      memoryFactEvidence(content, startOffset, endOffset, sourceActor, observedAt, memoryID, jobID),
			AssertionKind: "world_fact", ValidFrom: observedAt})
	}
	return out, nil
}

func (s *postgresDataStore) memoryFactSource(ctx context.Context, jobID int64) (content, observedAt string, memoryID int64, actor FactActor, err error) {
	err = s.db.QueryRow(ctx, `SELECT m.content,m.created_at,m.id,
 COALESCE(a.actor_principal,'system:model-inference'),COALESCE(a.transport_identity,'internal'),
 COALESCE(a.actor_role,'model'),COALESCE(a.authority_rank,10),COALESCE(a.authenticated,0)
FROM kb_async_jobs j JOIN memories m ON m.id=j.document_id
LEFT JOIN memory_fact_actors a ON a.memory_id=m.id
WHERE j.id=$1 AND j.kind='memory_facts'`, jobID).Scan(&content, &observedAt, &memoryID,
		&actor.Principal, &actor.TransportIdentity, &actor.Role, &actor.Rank, &actor.Authenticated)
	return
}

func factWorkHash(content, observed string, actor FactActor) string {
	raw, _ := json.Marshal(struct {
		Content, Observed string
		Actor             FactActor
	}{content, observed, actor})
	digest := sha256.Sum256(raw)
	return hex.EncodeToString(digest[:])
}

func (s *postgresDataStore) ClaimMemoryFact(ctx context.Context) (*MemoryFactWork, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB {
		return nil, errors.New("memory: fact claim requires KB transaction")
	}
	_, err := s.db.Exec(ctx, `UPDATE kb_async_jobs j SET status=CASE WHEN attempts >= $1 THEN 'failed' ELSE 'pending' END,
 claimed_by='',claimed_at='',last_error='expired extraction lease',updated_at=pg_now_text()
 WHERE kind='memory_facts' AND status='running' AND claimed_at<>'' AND claimed_at::timestamptz<clock_timestamp()-interval '15 minutes'
 AND EXISTS(SELECT 1 FROM memories m WHERE m.id=j.document_id)`, memoryFactMaxAttempts)
	if err != nil {
		return nil, err
	}
	for discarded := 0; discarded < 32; discarded++ {
		var work MemoryFactWork
		err = s.db.QueryRow(ctx, `SELECT m.id FROM memories m JOIN kb_async_jobs j ON j.document_id=m.id
 WHERE j.kind='memory_facts' AND j.status='pending' AND (j.next_attempt_at='' OR j.next_attempt_at::timestamptz<=clock_timestamp())
 ORDER BY j.id LIMIT 1 FOR UPDATE OF m SKIP LOCKED`).Scan(&work.MemoryID)
		if store.IsNoRows(err) {
			return nil, nil
		}
		if err != nil {
			return nil, err
		}
		var nonce [16]byte
		if _, err = cryptorand.Read(nonce[:]); err != nil {
			return nil, err
		}
		work.LeaseToken = hex.EncodeToString(nonce[:])
		err = s.db.QueryRow(ctx, `UPDATE kb_async_jobs SET status='running',claimed_by=$2,claimed_at=pg_now_text(),attempts=attempts+1,updated_at=pg_now_text()
 WHERE id=(SELECT id FROM kb_async_jobs WHERE kind='memory_facts' AND document_id=$1 AND status='pending'
 AND (next_attempt_at='' OR next_attempt_at::timestamptz<=clock_timestamp()) FOR UPDATE SKIP LOCKED)
 RETURNING id,generation,attempts`, work.MemoryID, work.LeaseToken).Scan(&work.JobID, &work.Generation, &work.Attempts)
		if store.IsNoRows(err) {
			continue
		}
		if err != nil {
			return nil, err
		}
		var observed string
		var actor FactActor
		work.Content, observed, work.MemoryID, actor, err = s.memoryFactSource(ctx, work.JobID)
		if err != nil {
			return nil, err
		}
		sourceErr := s.derivedSourcesAllowed(ctx, work.MemoryID)
		if sourceErr != nil && !errors.Is(sourceErr, errDerivedSource) {
			return nil, sourceErr
		}
		if sourceErr != nil || work.Content == "" {
			if _, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='done',claimed_by='',claimed_at='',last_error='',next_attempt_at='',updated_at=pg_now_text() WHERE id=$1`, work.JobID); err != nil {
				return nil, err
			}
			continue
		}
		if !validCapturedFactActor(actor) {
			actor = modelFactActor()
		}
		work.SourceHash = factWorkHash(work.Content, observed, actor)
		work.SystemPrompt = memoryFactPrompt()
		return &work, nil
	}
	return nil, errors.New("memory: stale extraction queue bound reached")
}

// CompleteMemoryFact validates the exact lease and source before deriving or
// writing anything. A late provider response can neither acknowledge a newer
// generation nor attach its claims to edited text.
func (s *postgresDataStore) CompleteMemoryFact(ctx context.Context, work MemoryFactWork, response string, success bool, reason string) (bool, error) {
	if _, ok := s.db.(store.Tx); !ok || s.placement != PlacementKB || work.JobID <= 0 || work.Generation <= 0 || len(work.LeaseToken) != 32 || len(work.SourceHash) != 64 {
		return false, errors.New("memory: invalid extraction completion")
	}
	var memoryID int64
	err := s.db.QueryRow(ctx, `SELECT m.id FROM memories m JOIN kb_async_jobs j ON j.document_id=m.id
 WHERE j.id=$1 AND j.kind='memory_facts' FOR UPDATE OF m`, work.JobID).Scan(&memoryID)
	if store.IsNoRows(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var generation int64
	var token, status string
	var attempts int
	err = s.db.QueryRow(ctx, `SELECT generation,claimed_by,status,attempts FROM kb_async_jobs WHERE id=$1 FOR UPDATE`, work.JobID).Scan(&generation, &token, &status, &attempts)
	if err != nil {
		return false, err
	}
	if generation != work.Generation || token != work.LeaseToken || status != "running" {
		return false, nil
	}
	content, observed, _, actor, err := s.memoryFactSource(ctx, work.JobID)
	if err != nil {
		return false, err
	}
	if !validCapturedFactActor(actor) {
		actor = modelFactActor()
	}
	if factWorkHash(content, observed, actor) != work.SourceHash {
		_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='pending',generation=generation+1,attempts=0,claimed_by='',claimed_at='',last_error='',next_attempt_at='',updated_at=pg_now_text() WHERE id=$1`, work.JobID)
		return false, err
	}
	sourceErr := s.derivedSourcesAllowed(ctx, memoryID)
	if sourceErr != nil && !errors.Is(sourceErr, errDerivedSource) {
		return false, sourceErr
	}
	if sourceErr == nil {
		candidates := patternFactCandidates(content, observed, memoryID, work.JobID, actor)
		if success {
			model, err := parseModelFactCandidates(response, content, observed, memoryID, work.JobID, actor)
			if err != nil {
				return false, err
			}
			candidates = append(candidates, model...)
		}
		for _, candidate := range candidates {
			decision := DecideFactWrite(FactWriteRequest{Head: candidate.SubjectKind, Relation: candidate.Relation, Tail: candidate.ObjectKind})
			if !decision.CommitAllowed {
				continue
			}
			if _, _, err := s.commitFactCandidate(ctx, candidate); err != nil {
				if errors.Is(err, errFactTombstoned) {
					continue
				}
				return false, err
			}
		}
	} else {
		success = true
	}
	if success {
		_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='done',last_error='',next_attempt_at='',claimed_by='',claimed_at='',updated_at=pg_now_text() WHERE id=$1`, work.JobID)
		return err == nil, err
	}
	unavailable := memoryFactProviderUnavailable(reason)
	nextStatus := "pending"
	if attempts >= memoryFactMaxAttempts && !unavailable {
		nextStatus = "failed"
	}
	next := time.Now().UTC().Add(memoryFactRetryDelay(attempts)).Format("2006-01-02 15:04:05")
	_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status=$2,last_error='typed-fact provider attempt failed',next_attempt_at=$3,
 attempts=CASE WHEN $4 AND attempts>0 THEN attempts-1 ELSE attempts END,claimed_by='',claimed_at='',updated_at=pg_now_text() WHERE id=$1`, work.JobID, nextStatus, next, unavailable)
	return err == nil, err
}
