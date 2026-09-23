package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

type briefingFact struct {
	RecallRecord
	EvidenceStrength float64 `json:"evidence_strength"`
	ObservationCount int     `json:"observation_count"`
	Salience         float64 `json:"salience"`
	LastSeenAt       string  `json:"last_seen_at"`
}

type briefingActivity struct {
	Source        *typedSourceVersion `json:"source_version,omitempty"`
	SessionID     string              `json:"session_id"`
	Summary       string              `json:"summary"`
	ReferenceTime string              `json:"reference_time"`
	CreatedAt     string              `json:"created_at"`
}

type briefingEntity struct {
	Name     string `json:"name"`
	Mentions int    `json:"mentions"`
	LastSeen string `json:"last_seen"`
}

type briefingBundle struct {
	Facts         []briefingFact     `json:"key_facts"`
	Activity      []briefingActivity `json:"recent_activity"`
	Entities      []briefingEntity   `json:"active_entities"`
	Style         string             `json:"style"`
	BriefingStyle string             `json:"briefing_style"`
	LimitTokens   int                `json:"limit_tokens"`
	ApproxTokens  int                `json:"approx_tokens"`
}

// Count the complete serialized payload, including metadata and the estimate
// itself. Remove whole trailing items, preserving UTF-8 and section priority.
func (b *briefingBundle) encodeBudgeted() ([]byte, error) {
	for {
		raw, err := json.Marshal(b)
		if err != nil {
			return nil, err
		}
		tokens := (len(raw) + 3) / 4
		if b.ApproxTokens != tokens {
			b.ApproxTokens = tokens
			continue
		}
		if tokens <= b.LimitTokens {
			return raw, nil
		}
		switch {
		case len(b.Entities) > 0:
			b.Entities = b.Entities[:len(b.Entities)-1]
		case len(b.Activity) > 0:
			b.Activity = b.Activity[:len(b.Activity)-1]
		case len(b.Facts) > 0:
			b.Facts = b.Facts[:len(b.Facts)-1]
		default:
			return nil, fmt.Errorf("memory: briefing envelope exceeds token budget")
		}
	}
}

func (s *postgresDataStore) BriefingBundle(ctx context.Context, tokens int) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	style := "compact"
	// A promotion is optional metadata. Its lookup cannot abort the transaction
	// or turn a missing promotion into an empty successful memory result.
	_ = s.retrievalPolicyAttempt(ctx, func() error {
		var promoted string
		err := s.db.QueryRow(ctx, `SELECT arm_id FROM bandit_promotions WHERE decision_point='briefing_style'`).Scan(&promoted)
		if store.IsNoRows(err) {
			return nil
		}
		if err == nil && (promoted == "compact" || promoted == "evidence_heavy") {
			style = promoted
		}
		return err
	})
	if tokens <= 0 {
		tokens = 1500
		if style == "evidence_heavy" {
			tokens = 3000
		}
	}
	factLimit, activityLimit, entityLimit := 30, 5, 20
	if style == "compact" {
		tokens = min(tokens, 1024)
	} else {
		// A learned style may broaden the candidate pool, but cannot raise
		// the caller's allocation. Only an unspecified budget uses its default.
		factLimit, activityLimit, entityLimit = 60, 10, 40
	}
	b := briefingBundle{Facts: []briefingFact{}, Activity: []briefingActivity{}, Entities: []briefingEntity{}, Style: style, BriefingStyle: style, LimitTokens: min(max(tokens, 64), 8192)}
	rows, err := s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence,
 evidence_strength,observation_count,COALESCE(NULLIF(last_used_at,''),updated_at),
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1),record_revision::text
 FROM memories WHERE `+currentMemorySQL("")+`
 AND tier IN ('L2','L3','L4','L5') AND kind<>'scratch' AND COALESCE(sensitivity,'normal')<>'secret'
 ORDER BY `+queryScopeOrder+`,(confidence+evidence_strength) DESC,observation_count DESC,use_count DESC,id DESC LIMIT $1`, factLimit)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var f briefingFact
		r := &f.Record
		r.Version = &MemoryRecordVersion{SchemaVersion: 1}
		if err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &f.EvidenceStrength, &f.ObservationCount, &f.LastSeenAt, &r.Version.OwnerID, &r.Version.RecordRevision); err != nil {
			rows.Close()
			return nil, err
		}
		r.Version.RecordID = strconv.FormatInt(r.ID, 10)
		if !r.Version.validFor(r.ID) {
			rows.Close()
			return nil, fmt.Errorf("memory: briefing source version unavailable")
		}
		f.RecallRecord = recallItems([]Record{*r})[0]
		f.Salience = r.Confidence + f.EvidenceStrength + 0.2*math.Log1p(float64(max(f.ObservationCount, 0)))
		b.Facts = append(b.Facts, f)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	rows, err = s.db.Query(ctx, `WITH ranked AS (
 SELECT e.source_session,e.episode_text,e.reference_time,e.created_at,
 e.id::text AS episode_id,e.record_revision::text AS episode_revision,
 m.id::text AS parent_id,m.record_revision::text AS parent_revision,
 (SELECT owner_id::text FROM memory_collection_owner WHERE id=1) AS owner_id,`+queryScopeOrder+` AS scope_rank,
 row_number() OVER(PARTITION BY e.source_session ORDER BY `+queryScopeOrder+`,e.created_at DESC,e.id DESC) AS rn
 FROM memory_episodes e JOIN memories m ON m.id=e.memory_id
 WHERE e.source_session<>'' AND `+currentMemorySQL("m.")+` AND `+currentEpisodeInputsSQL("e")+`
 AND COALESCE(m.sensitivity,'normal')<>'secret')
 SELECT source_session,COALESCE(episode_text,''),COALESCE(reference_time,''),COALESCE(created_at,''),
 episode_id,episode_revision,parent_id,parent_revision,owner_id FROM ranked WHERE rn=1
 ORDER BY scope_rank,created_at DESC,source_session DESC LIMIT $1`, activityLimit)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var a briefingActivity
		source := &typedSourceVersion{Kind: "memory_episode", Version: MemoryRecordVersion{SchemaVersion: 1}, MemoryParents: []MemoryRecordVersion{{SchemaVersion: 1}}, MemoryParentState: "observed"}
		parent := &source.MemoryParents[0]
		if err = rows.Scan(&a.SessionID, &a.Summary, &a.ReferenceTime, &a.CreatedAt,
			&source.Version.RecordID, &source.Version.RecordRevision, &parent.RecordID, &parent.RecordRevision, &source.Version.OwnerID); err != nil {
			rows.Close()
			return nil, err
		}
		parent.OwnerID = source.Version.OwnerID
		if !validTypedSource(typedProjectionRef{Channel: "episodes", ID: source.Version.RecordID, Source: source}) {
			rows.Close()
			return nil, fmt.Errorf("memory: briefing episode source version unavailable")
		}
		a.Source = source
		b.Activity = append(b.Activity, a)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	rows, err = s.db.Query(ctx, `SELECT entity,COUNT(*),MAX(COALESCE(NULLIF(m.last_used_at,''),m.updated_at))
 FROM memory_entities me JOIN memories m ON m.id=me.memory_id
 WHERE entity<>'' AND `+currentMemorySQL("m.")+`
 AND COALESCE(m.sensitivity,'normal')<>'secret'
 AND (NULLIF(m.last_used_at,'') IS NULL OR aimee_utc_text_timestamptz(m.last_used_at)>=now()-interval '30 days')
 GROUP BY entity ORDER BY MIN(`+queryScopeOrder+`),COUNT(*) DESC,
 MAX(COALESCE(NULLIF(m.last_used_at,''),m.updated_at)) DESC,entity LIMIT $1`, entityLimit)
	if err != nil {
		return nil, err
	}
	for rows.Next() {
		var e briefingEntity
		if err = rows.Scan(&e.Name, &e.Mentions, &e.LastSeen); err != nil {
			rows.Close()
			return nil, err
		}
		b.Entities = append(b.Entities, e)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	return b.encodeBudgeted()
}
