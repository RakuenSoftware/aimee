package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"time"
)

type staleAlert struct {
	MemoryID   int64   `json:"memory_id"`
	Text       string  `json:"text"`
	CreatedAt  string  `json:"created_at"`
	TTLAt      string  `json:"ttl_at"`
	AgeDays    float64 `json:"age_days"`
	WindowDays float64 `json:"window_days"`
}
type conflictAlert struct {
	Conflict
	ConflictID int64   `json:"conflict_id"`
	MemoryIDs  []int64 `json:"memory_ids"`
	Topic      string  `json:"topic"`
	A          string  `json:"a"`
	B          string  `json:"b"`
}
type supersededAlert struct {
	Record
	MemoryID     int64  `json:"memory_id"`
	Text         string `json:"text"`
	SupersededAt string `json:"superseded_at"`
}
type alertsBundle struct {
	Stale      []staleAlert      `json:"stale_pending"`
	Conflicts  []conflictAlert   `json:"unresolved_contradictions"`
	Superseded []supersededAlert `json:"newly_superseded"`
	ElapsedMS  float64           `json:"elapsed_ms"`
}

func (s *postgresDataStore) AlertsBundle(ctx context.Context, since string) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	start := time.Now()
	if since != "" {
		when, err := parseMemoryTime(since)
		if err != nil {
			return nil, err
		}
		since = when.UTC().Format(time.RFC3339Nano)
	}
	b := alertsBundle{Stale: []staleAlert{}, Conflicts: []conflictAlert{}, Superseded: []supersededAlert{}}
	rows, err := s.db.Query(ctx, `SELECT id,content,created_at,ttl_at,
 EXTRACT(EPOCH FROM now()-aimee_utc_text_timestamptz(created_at))/86400.0,
 EXTRACT(EPOCH FROM aimee_utc_text_timestamptz(NULLIF(ttl_at,''))-aimee_utc_text_timestamptz(created_at))/86400.0
 FROM memories WHERE lifecycle_state='pending' AND NULLIF(ttl_at,'') IS NOT NULL
 AND aimee_utc_text_timestamptz(NULLIF(ttl_at,''))>aimee_utc_text_timestamptz(created_at)
 AND now()-aimee_utc_text_timestamptz(created_at)>=0.8*(aimee_utc_text_timestamptz(NULLIF(ttl_at,''))-aimee_utc_text_timestamptz(created_at))
 ORDER BY `+queryScopeOrder+`,aimee_utc_text_timestamptz(created_at),id DESC LIMIT 50`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item staleAlert
		if err = rows.Scan(&item.MemoryID, &item.Text, &item.CreatedAt, &item.TTLAt, &item.AgeDays, &item.WindowDays); err != nil {
			rows.Close()
			return nil, err
		}
		b.Stale = append(b.Stale, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	// Both joined parents are filtered by the same runtime RLS context. A
	// partially visible or orphaned conflict must not reveal the hidden side.
	rankA := strings.ReplaceAll(strings.ReplaceAll(queryScopeOrder, "scope_type", "ma.scope_type"), "scope_value", "ma.scope_value")
	rankB := strings.ReplaceAll(strings.ReplaceAll(queryScopeOrder, "scope_type", "mb.scope_type"), "scope_value", "mb.scope_value")
	rows, err = s.db.Query(ctx, `SELECT c.id,c.memory_a,c.memory_b,c.detected_at,COALESCE(c.resolution,''),ma.key,ma.content,mb.content
 FROM memory_conflicts c JOIN memories ma ON ma.id=c.memory_a JOIN memories mb ON mb.id=c.memory_b
 WHERE c.resolved=0 ORDER BY LEAST(`+rankA+`,`+rankB+`),c.detected_at DESC,c.id DESC LIMIT 50`)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item conflictAlert
		if err = rows.Scan(&item.ID, &item.MemoryAID, &item.MemoryBID, &item.DetectedAt, &item.Resolution, &item.Topic, &item.A, &item.B); err != nil {
			rows.Close()
			return nil, err
		}
		item.ConflictID = item.ID
		item.MemoryIDs = []int64{item.MemoryAID, item.MemoryBID}
		b.Conflicts = append(b.Conflicts, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	rows, err = s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence,updated_at
 FROM memories WHERE lifecycle_state='superseded'
 AND aimee_utc_text_timestamptz(updated_at)>=COALESCE(aimee_utc_text_timestamptz(NULLIF($1,'')),now()-interval '7 days')
 ORDER BY `+queryScopeOrder+`,aimee_utc_text_timestamptz(updated_at) DESC,id DESC LIMIT 50`, since)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var item supersededAlert
		if err = rows.Scan(&item.ID, &item.Scope.Type, &item.Scope.Value, &item.Tier, &item.Kind, &item.Key, &item.Content, &item.Confidence, &item.SupersededAt); err != nil {
			rows.Close()
			return nil, err
		}
		item.MemoryID, item.Text = item.ID, item.Content
		b.Superseded = append(b.Superseded, item)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	b.ElapsedMS = float64(time.Since(start)) / float64(time.Millisecond)
	return json.Marshal(b)
}

func alertsText(b alertsBundle) string {
	var out strings.Builder
	fmt.Fprintf(&out, "# Memory Alerts\n\n## Stale Pending (%d)\n", len(b.Stale))
	for _, a := range b.Stale {
		fmt.Fprintf(&out, "  - #%d age=%.1fd/%.1fd: %s\n", a.MemoryID, a.AgeDays, a.WindowDays, a.Text)
	}
	fmt.Fprintf(&out, "\n## Unresolved Contradictions (%d)\n", len(b.Conflicts))
	for _, a := range b.Conflicts {
		fmt.Fprintf(&out, "  - %s\n    A: %s\n    B: %s\n", a.Topic, a.A, a.B)
	}
	fmt.Fprintf(&out, "\n## Newly Superseded (%d)\n", len(b.Superseded))
	for _, a := range b.Superseded {
		fmt.Fprintf(&out, "  - #%d [%s] %s (at %s)\n", a.MemoryID, a.Key, a.Text, a.SupersededAt)
	}
	fmt.Fprintf(&out, "\nassembled in %.2fms\n", b.ElapsedMS)
	return out.String()
}
