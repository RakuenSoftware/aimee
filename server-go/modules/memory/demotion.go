package memory

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strconv"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

type DemotionConfig struct {
	Enabled      int     `json:"enabled"`
	Minimum      int     `json:"n_min"`
	Window       int     `json:"window"`
	HalfLifeDays float64 `json:"half_life_days"`
}

type demotionSummary struct {
	// The legacy endpoint counted profiles plus live actions in this field.
	ProfilesWritten int    `json:"profiles_written"`
	ProfilesCreated int    `json:"profiles_created"`
	Demoted         int    `json:"demoted"`
	Scored          int    `json:"scored"`
	Skipped         bool   `json:"skipped"`
	Status          string `json:"status"`
}

type demotionEvidence struct {
	id, created string
	payload     json.RawMessage
}

func demotionScore(evidence []demotionEvidence, minimum int, halfLife float64, now time.Time) (float64, bool) {
	if minimum <= 0 {
		minimum = 5
	}
	if halfLife <= 0 {
		halfLife = 30
	}
	score, valid := 0.0, 0
	for _, row := range evidence {
		var payload map[string]json.RawMessage
		var verdict string
		if json.Unmarshal(row.payload, &payload) != nil || json.Unmarshal(payload["verdict"], &verdict) != nil || string(payload["verdict"]) == "null" {
			continue
		}
		weight := 1.0
		if raw := payload["weight"]; len(raw) > 0 && (raw[0] == '-' || raw[0] >= '0' && raw[0] <= '9') {
			if json.Unmarshal(raw, &weight) != nil || math.IsInf(weight, 0) || math.IsNaN(weight) {
				continue
			}
		}
		sign := 0.0
		switch verdict {
		case "accepted":
			sign = 1
		case "corrected", "contradicted", "rolled_back":
			sign = -1
		}
		age := 0.0
		if created, err := parseMemoryTime(row.created); err == nil && created.Unix() > 0 && now.After(created) {
			age = now.Sub(created).Hours() / 24
		}
		score += sign * weight * math.Exp(-math.Ln2*age/halfLife)
		valid++
	}
	return score, valid >= minimum && !math.IsNaN(score) && !math.IsInf(score, 0)
}

func demotionPercentile(sorted []float64, p float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	index := p * float64(len(sorted)-1)
	lo := int(index)
	if lo+1 >= len(sorted) {
		return sorted[len(sorted)-1]
	}
	return sorted[lo]*(1-(index-float64(lo))) + sorted[lo+1]*(index-float64(lo))
}

// Profiles have always persisted four decimal places; live admission uses that
// persisted threshold rather than an unrounded, subtly different percentile.
func demotionRounded(value float64) float64 {
	n, _ := strconv.ParseFloat(strconv.FormatFloat(value, 'f', 4, 64), 64)
	return n
}

func demotionArtifactID() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x-%x-%x-%x-%x", raw[:4], raw[4:6], raw[6:8], raw[8:10], raw[10:]), nil
}

func (s *postgresDataStore) demotionEvidence(ctx context.Context, id int64, window int) ([]demotionEvidence, error) {
	rows, err := s.db.Query(ctx, `SELECT id,payload::text,created_at FROM artifacts
 WHERE kind='retrieval_attribution' AND scope_id=$1 ORDER BY created_at DESC,id LIMIT $2`, strconv.FormatInt(id, 10), window)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	evidence := []demotionEvidence{}
	ids := []string{}
	for rows.Next() {
		var row demotionEvidence
		var raw string
		if err := rows.Scan(&row.id, &raw, &row.created); err != nil {
			return nil, err
		}
		row.payload = json.RawMessage(raw)
		evidence = append(evidence, row)
		ids = append(ids, row.id)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	rows.Close()
	if len(ids) > 0 {
		encoded, _ := json.Marshal(ids)
		if _, err := s.db.Exec(ctx, `UPDATE artifacts SET last_accessed_at=CURRENT_TIMESTAMP
 WHERE id IN (SELECT jsonb_array_elements_text($1::jsonb))`, string(encoded)); err != nil {
			return nil, err
		}
	}
	return evidence, nil
}

// Runs inside handleData's scoped transaction. Profiles, evidence touches,
// confidence changes and their action artifacts either all commit or all roll
// back. A concurrent run is skipped while another transaction holds the lock.
func (s *postgresDataStore) runDemotion(ctx context.Context, config DemotionConfig) (demotionSummary, error) {
	result := demotionSummary{Status: "ok"}
	if config.Enabled == 0 {
		return result, nil
	}
	var acquired bool
	if err := s.db.QueryRow(ctx, `SELECT pg_try_advisory_xact_lock(hashtextextended('memory:demotion-run',0))`).Scan(&acquired); err != nil {
		return result, err
	}
	if !acquired {
		result.Skipped = true
		return result, nil
	}
	window := config.Window
	if window <= 0 {
		window = 64
	}
	rows, err := s.db.Query(ctx, `SELECT scope_id FROM artifacts WHERE kind='retrieval_attribution'
 GROUP BY scope_id HAVING COUNT(*) >= $1 ORDER BY scope_id LIMIT 4096`, max(1, config.Minimum))
	if err != nil {
		return result, err
	}
	ids := []int64{}
	candidates := 0
	for rows.Next() {
		var text string
		if err := rows.Scan(&text); err != nil {
			rows.Close()
			return result, err
		}
		candidates++
		if id, err := strconv.ParseInt(text, 10, 64); err == nil && id > 0 {
			ids = append(ids, id)
		}
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return result, err
	}
	type scored struct {
		id    int64
		kind  string
		score float64
	}
	scoredRows := []scored{}
	classes := map[string][]float64{}
	order := []string{}
	now := time.Now().UTC()
	for _, id := range ids {
		evidence, err := s.demotionEvidence(ctx, id, window)
		if err != nil {
			return result, err
		}
		score, ok := demotionScore(evidence, config.Minimum, config.HalfLifeDays, now)
		if !ok {
			continue
		}
		var kind string
		err = s.db.QueryRow(ctx, `SELECT kind FROM memories WHERE id=$1 FOR UPDATE`, id).Scan(&kind)
		if store.IsNoRows(err) {
			continue
		}
		if err != nil {
			return result, err
		}
		if kind == "" {
			continue
		}
		if _, exists := classes[kind]; !exists {
			order = append(order, kind)
		}
		classes[kind] = append(classes[kind], score)
		scoredRows = append(scoredRows, scored{id, kind, score})
	}
	result.Scored = len(scoredRows)
	thresholds := map[string]float64{}
	for _, kind := range order {
		scores := classes[kind]
		sort.Float64s(scores)
		percentiles := map[string]float64{}
		for _, point := range []int{10, 25, 50, 75, 90} {
			percentiles[fmt.Sprintf("p%d", point)] = demotionRounded(demotionPercentile(scores, float64(point)/100))
		}
		payload, err := json.Marshal(map[string]any{"memory_class": kind, "n_rows_scored": len(scores), "n_candidates": candidates, "score_percentiles": percentiles})
		if err != nil {
			return result, err
		}
		id, err := demotionArtifactID()
		if err != nil {
			return result, err
		}
		_, err = s.db.Exec(ctx, `INSERT INTO artifacts(id,kind,state,scope_kind,scope_id,confidence,target_surface,created_at,committed_at,last_accessed_at,payload)
 VALUES($1,'demotion_profile','committed','global','',1,$2,pg_now_text(),pg_now_text(),CASE WHEN $4 THEN CURRENT_TIMESTAMP ELSE NULL END,$3::jsonb)`, id, kind, string(payload), config.Enabled >= 2)
		if err != nil {
			return result, err
		}
		thresholds[kind] = percentiles["p10"]
		result.ProfilesCreated++
	}
	if config.Enabled >= 2 {
		for _, row := range scoredRows {
			if row.score >= thresholds[row.kind] {
				continue
			}
			updated, err := s.DemoteConfidence(ctx, row.id)
			if err != nil {
				return result, err
			}
			if !updated {
				continue
			}
			payload, _ := json.Marshal(map[string]any{"row_id": row.id, "kind": row.kind, "score": demotionRounded(row.score), "p10": thresholds[row.kind]})
			id, err := demotionArtifactID()
			if err != nil {
				return result, err
			}
			_, err = s.db.Exec(ctx, `INSERT INTO artifacts(id,kind,state,scope_kind,scope_id,confidence,created_at,payload)
 VALUES($1,'demotion_action','demoted','memory',$2,0,pg_now_text(),$3::jsonb)`, id, strconv.FormatInt(row.id, 10), string(payload))
			if err != nil {
				return result, err
			}
			result.Demoted++
		}
	}
	result.ProfilesWritten = result.ProfilesCreated + result.Demoted
	return result, nil
}
