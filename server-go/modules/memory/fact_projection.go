package memory

import (
	"context"
	"crypto/sha256"
	"fmt"
)

// Metadata travels beside the unchanged plain-text facts block. Its digest
// binds the exact selected bytes and observed versions, not current release
// authorization or authenticated producer evidence.
type factProjection struct {
	SchemaVersion      int                  `json:"schema_version"`
	ProjectionDigest   string               `json:"projection_digest"`
	SelectionDigest    string               `json:"selection_digest"`
	RenderedBytes      int                  `json:"rendered_bytes"`
	Retained           []typedProjectionRef `json:"retained_items"`
	SourceVersionState string               `json:"source_version_state"`
}

func newFactProjection(block string, refs []typedProjectionRef) *factProjection {
	if refs == nil {
		refs = []typedProjectionRef{}
	}
	p := &factProjection{SchemaVersion: 1, ProjectionDigest: fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(block))),
		RenderedBytes: len(block), Retained: refs, SourceVersionState: typedSourceVersionState(refs)}
	p.SelectionDigest = typedSelectionDigest(p.ProjectionDigest, refs)
	return p
}

func (p *factProjection) valid(block string) bool {
	if p == nil || p.SchemaVersion != 1 || !validFactSources(block, p.Retained) {
		return false
	}
	expected := newFactProjection(block, p.Retained)
	return p.ProjectionDigest == expected.ProjectionDigest && p.SelectionDigest == expected.SelectionDigest && p.RenderedBytes == expected.RenderedBytes && p.SourceVersionState == expected.SourceVersionState
}

func validFactSources(block string, refs []typedProjectionRef) bool {
	if refs == nil || len(refs) > factRecallMaxFacts*(factRecallMaxEntities+1) || (block == "") != (len(refs) == 0) {
		return false
	}
	seen := map[string]bool{}
	owner := ""
	for _, ref := range refs {
		if ref.Channel != "facts" || ref.Source == nil || ref.Source.MemoryParentState != "observed" || !validTypedSource(ref) || seen[ref.ID] {
			return false
		}
		if owner != "" && ref.Source.Version.OwnerID != owner {
			return false
		}
		owner = ref.Source.Version.OwnerID
		seen[ref.ID] = true
	}
	return true
}

func (s *postgresDataStore) RecallFactProjection(ctx context.Context, entity, query string, sensitive bool, capacity int) (string, int, *factProjection, error) {
	refs := []typedProjectionRef{}
	block, count, err := s.recallFactsSources(ctx, entity, query, sensitive, capacity, &refs)
	if err != nil {
		return "", 0, nil, err
	}
	if !validFactSources(block, refs) {
		return "", 0, nil, fmt.Errorf("memory: invalid fact source projection")
	}
	return block, count, newFactProjection(block, refs), nil
}
