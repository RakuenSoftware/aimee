package memory

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
)

// EvaluationManifest binds comparable inputs and effective policies. It does
// not assert that the measured implementation meets the wider release gates.
type EvaluationManifest struct {
	Version            int      `json:"version"`
	CorpusSHA256       string   `json:"corpus_sha256"`
	SchemaSHA256       string   `json:"schema_sha256"`
	EmbeddingIdentity  string   `json:"embedding_identity"`
	EmbeddingDimension int      `json:"embedding_dimension"`
	PolicySHA256       string   `json:"policy_sha256"`
	CaseIDs            []string `json:"case_ids"`
}

type EvaluationCaseResult struct {
	ID        string           `json:"id"`
	Expected  []string         `json:"expected_fids"`
	Retrieved []string         `json:"retrieved_fids"`
	Scores    EvaluationScores `json:"metrics"`
	LatencyMS float64          `json:"owner_latency_ms"`
}

func EvaluationDigest(data []byte) string { return fmt.Sprintf("%x", sha256.Sum256(data)) }

func (m EvaluationManifest) Validate() error {
	digest := func(s string) bool {
		if len(s) != 64 {
			return false
		}
		for _, c := range s {
			if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
				return false
			}
		}
		return true
	}
	if m.Version != 1 || !digest(m.CorpusSHA256) || !digest(m.SchemaSHA256) || !digest(m.PolicySHA256) || m.EmbeddingIdentity == "" || m.EmbeddingDimension < 1 || m.EmbeddingDimension > 2000 || len(m.CaseIDs) == 0 {
		return errors.New("evaluation baseline requires a complete versioned manifest; regenerate an unbound legacy baseline explicitly")
	}
	seen := map[string]bool{}
	for _, id := range m.CaseIDs {
		if id == "" || seen[id] {
			return errors.New("evaluation manifest has missing or duplicate case IDs")
		}
		seen[id] = true
	}
	return nil
}

func (m *EvaluationModule) evaluationManifest(ctx context.Context, corpus EvaluationCorpus) (EvaluationManifest, error) {
	raw, err := json.Marshal(corpus)
	if err != nil {
		return EvaluationManifest{}, err
	}
	result := EvaluationManifest{Version: 1, CorpusSHA256: EvaluationDigest(raw)}
	err = m.backend.db.QueryRow(ctx, `SELECT v.serving_id,v.dimension FROM memory_active_embedder a JOIN memory_embedder_versions v ON v.version=a.version WHERE a.id=1`).Scan(&result.EmbeddingIdentity, &result.EmbeddingDimension)
	if err != nil {
		return result, err
	}
	for _, row := range corpus.Cases {
		result.CaseIDs = append(result.CaseIDs, row.ID)
	}
	request, err := m.backend.planRecall(DataRequest{Query: "manifest", Limit: 20})
	if err != nil {
		return result, err
	}
	var settings map[string]any
	if m.backend.settings != nil {
		settings, err = m.backend.settings()
		if err != nil {
			return result, err
		}
	}
	raw, err = json.Marshal(map[string]any{
		"retrieval": "raw-query-versioned-rrf60-v1", "eligibility": currentEligibilityPolicy, "pagerank_policy": pageRankRecallPolicy,
		"pagerank_enabled": request.pageRankConfig.enabled, "pagerank": request.pageRankConfig.request,
		"graph_fusion": m.backend.graphFusionEnabled(), "settings": settings,
		"candidate_limit": 20, "placement": "kb", "fixture_policy": "full-text-raw-query-v1",
	})
	if err != nil {
		return result, err
	}
	result.PolicySHA256 = EvaluationDigest(raw)
	return result, nil
}
