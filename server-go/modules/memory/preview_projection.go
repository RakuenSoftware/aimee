package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
)

// The wire commitment has the same fields as fact projections, with a distinct
// channel and validator. It binds the rendered preview, not unused full content.
type previewProjection factProjection

func renderMemoryPreview(row ingressMemoryPreview) (body, preview string, missing bool, err error) {
	id, err := strconv.ParseInt(row.ID, 10, 64)
	if err != nil {
		return "", "", false, fmt.Errorf("invalid memory preview identity")
	}
	if id <= 0 {
		return "", "", false, nil
	}
	preview, missing = row.Headline, row.Headline == ""
	if missing {
		preview = row.Content
	}
	body = fmt.Sprintf("  - memory:%d", id)
	if row.Key != "" {
		body += " " + ingressSingleLine(row.Key, 80)
	}
	tier, kind := row.Tier, row.Kind
	if tier == "" {
		tier = "?"
	}
	if kind == "" {
		kind = "memory"
	}
	score := row.ScoreText
	if score == "" {
		score = fmt.Sprintf("%.3f", row.Score)
	} else {
		value, parseErr := strconv.ParseFloat(score, 64)
		if len(score) > 384 || parseErr != nil || math.IsNaN(value) || math.IsInf(value, 0) || fmt.Sprintf("%.3f", value) != score {
			return "", "", false, errors.New("memory: invalid preview score")
		}
	}
	body += fmt.Sprintf(" [%s/%s score=%s headline_missing=%t]\n", tier, kind, score, missing)
	preview = ingressSingleLine(preview, 220)
	if preview != "" {
		body += "    > " + preview + "\n"
	}
	return body, preview, missing, nil
}

func newPreviewProjection(rows []ingressMemoryPreview) (*previewProjection, error) {
	if len(rows) > 64 {
		return nil, errors.New("memory: too many previews")
	}
	refs := []typedProjectionRef{}
	var block strings.Builder
	seen := map[string]bool{}
	owner := ""
	for _, row := range rows {
		if row.Source == nil {
			return nil, errors.New("memory: preview source unavailable")
		}
		ref := typedProjectionRef{Channel: "memory_previews", ID: row.Source.Version.RecordID, Source: row.Source}
		if !validTypedSource(ref) || row.Source.MemoryParentState != "observed" || seen[row.ID] || (owner != "" && owner != row.Source.Version.OwnerID) {
			return nil, errors.New("memory: invalid preview source")
		}
		if row.Headline == "" {
			if row.Source.Kind != "memory_record" || ref.ID != row.ID {
				return nil, errors.New("memory: invalid content source")
			}
		} else if row.Source.Kind != "memory_summary" || row.Source.MemoryParents[0].RecordID != row.ID {
			return nil, errors.New("memory: invalid headline source")
		}
		body, _, _, err := renderMemoryPreview(row)
		if err != nil || body == "" {
			return nil, errors.New("memory: invalid preview")
		}
		seen[row.ID], owner = true, row.Source.Version.OwnerID
		refs = append(refs, ref)
		block.WriteString(body)
	}
	return (*previewProjection)(newFactProjection(block.String(), refs)), nil
}

func (p *previewProjection) valid(rows []ingressMemoryPreview) bool {
	if p == nil || p.SchemaVersion != 1 {
		return false
	}
	expected, err := newPreviewProjection(rows)
	return err == nil && p.ProjectionDigest == expected.ProjectionDigest && p.SelectionDigest == expected.SelectionDigest &&
		p.RenderedBytes == expected.RenderedBytes && p.SourceVersionState == expected.SourceVersionState &&
		p.SelectionDigest == typedSelectionDigest(p.ProjectionDigest, p.Retained)
}

// Rehydrate the selected canonical rows and the exact headline in one snapshot.
// Ranking remains the candidate step. Ingress needs no per-row epistemic query
// or the rest of the public diagnostic metadata.
func (s *postgresDataStore) ingressMemoryPreviews(ctx context.Context, diagnostics []Diagnostic, exact Scope) ([]ingressMemoryPreview, *previewProjection, error) {
	result := []ingressMemoryPreview{}
	if len(diagnostics) == 0 {
		p, err := newPreviewProjection(result)
		return result, p, err
	}
	ids := make([]int64, len(diagnostics))
	for i, d := range diagnostics {
		ids[i] = d.Memory.ID
	}
	rows, err := s.db.Query(ctx, `SELECT m.id::text,m.key,m.tier,m.kind,m.content,m.record_revision::text,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 COALESCE(h.id::text,''),COALESCE(h.record_revision::text,''),COALESCE(h.summary,'')
 FROM memories m LEFT JOIN LATERAL (
 SELECT id,record_revision,summary FROM
 (SELECT id,record_revision,scope,summary FROM memory_summaries summary WHERE summary.memory_id=m.id AND `+summaryCurrentInputsSQL("summary", "m")+` ORDER BY id LIMIT 4) summaries
 ORDER BY CASE WHEN scope='headline' AND summary<>'' THEN 0 ELSE 1 END,id LIMIT 1
 ) h ON TRUE WHERE m.id=ANY($1::text::bigint[]) AND `+currentMemorySQL("m.")+` AND ($2::text='' OR (m.scope_type=$2 AND m.scope_value=$3))`, memoryIDsParameter(ids), exact.Type, exact.Value)
	if err != nil {
		return nil, nil, err
	}
	defer rows.Close()
	byID := map[string]ingressMemoryPreview{}
	for rows.Next() {
		var row ingressMemoryPreview
		var revision, owner, summaryID, summaryRevision string
		if err := rows.Scan(&row.ID, &row.Key, &row.Tier, &row.Kind, &row.Content, &revision, &owner, &summaryID, &summaryRevision, &row.Headline); err != nil {
			return nil, nil, err
		}
		version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: row.ID, RecordRevision: revision}
		row.Source = &typedSourceVersion{Kind: "memory_record", Version: version, MemoryParentState: "observed"}
		if row.Headline != "" {
			row.Source = &typedSourceVersion{Kind: "memory_summary", Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: summaryID, RecordRevision: summaryRevision}, MemoryParents: []MemoryRecordVersion{version}, MemoryParentState: "observed"}
		}
		byID[row.ID] = row
	}
	if err := rows.Err(); err != nil {
		return nil, nil, err
	}
	for _, d := range diagnostics {
		row, ok := byID[strconv.FormatInt(d.Memory.ID, 10)]
		if !ok {
			return nil, nil, errors.New("memory: selected preview is no longer eligible")
		}
		row.Score = d.Parts.Total
		row.ScoreText = fmt.Sprintf("%.3f", row.Score)
		row.Preview = row.Headline
		if row.Preview == "" {
			row.Preview = row.Content
		}
		result = append(result, row)
	}
	projection, err := newPreviewProjection(result)
	return result, projection, err
}

func decodePreviewProjection(raw json.RawMessage, rows []ingressMemoryPreview) (*previewProjection, error) {
	var projection *previewProjection
	if json.Unmarshal(raw, &projection) != nil || !projection.valid(rows) {
		return nil, &contextBudgetError{"invalid_projection", "memory preview source or rendered bytes mismatch"}
	}
	return projection, nil
}
