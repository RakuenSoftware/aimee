package memory

import (
	"context"
	"errors"
	"strconv"
	"time"
)

// Load only metadata visible through this request's existing storage capability.
// The caller's already observed version must still match; this observation never
// becomes a substitute for release-time reauthorization.
func (s *postgresDataStore) utilityHorizonDecision(ctx context.Context, id int64, version *MemoryRecordVersion, purpose string, baseEligible bool) (*horizonDecision, error) {
	c, err := configuredUtilityHorizon()
	if err != nil || c == nil {
		return nil, err
	}
	personal := s.placement == PlacementServer
	table := "memories"
	if personal {
		table = "user_memories"
	}
	r := horizonRecord{BaseEligible: baseEligible, Version: MemoryRecordVersion{SchemaVersion: 1, RecordID: strconv.FormatInt(id, 10)}}
	var now, created, confirmed string
	var generation string
	scope := ""
	if !personal {
		scope = " AND event.scope_type=m.scope_type AND event.scope_value=m.scope_value"
	}
	err = s.db.QueryRow(ctx, `SELECT m.kind,`+horizonDomainSQL("m.", personal)+`,m.record_revision::text,
 (SELECT owner_id::text FROM `+horizonOwnerTable(personal)+` WHERE id=1),to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),
 COALESCE(to_char(`+horizonAnchorSQL("m.", personal, horizonRule{Anchor: "created"}, "")+` AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.US"Z"'),''),
 COALESCE((SELECT to_char(min(event.recorded_at) AT TIME ZONE 'UTC','YYYY-MM-DD"T"HH24:MI:SS.US"Z"') FROM `+horizonJournalTable(personal)+` event WHERE event.memory_id=m.id AND event.record_revision=m.record_revision AND event.operation='update'`+scope+`),''),
 COALESCE((SELECT min(event.generation)::text FROM `+horizonJournalTable(personal)+` event WHERE event.memory_id=m.id AND event.record_revision=m.record_revision AND event.operation='update'`+scope+`),'')
 FROM `+table+` m WHERE m.id=$1`, id).Scan(&r.Kind, &r.Domain, &r.Version.RecordRevision, &r.Version.OwnerID, &now, &created, &confirmed, &generation)
	if err != nil {
		return nil, err
	}
	if version == nil || *version != r.Version {
		return nil, errors.New("memory: horizon snapshot version changed")
	}
	if created != "" {
		r.Created = horizonAnchor{At: created, EventID: "creation:" + r.Version.OwnerID + ":" + r.Version.RecordID, Version: r.Version}
	}
	for _, o := range c.Overrides {
		if o.Version != r.Version {
			continue
		}
		rule := o.Rule
		r.Override = &rule
		if generation != "" && generation == o.ConfirmationGeneration && confirmed != "" {
			r.Confirmed = &horizonAnchor{At: confirmed, EventID: "confirmation:" + r.Version.OwnerID + ":" + generation, Version: r.Version}
		}
		break
	}
	clock, _ := time.Parse(time.RFC3339Nano, now)
	decision := evaluateUtilityHorizon(r, c.Policy, purpose, clock)
	decision.Mode = c.Mode
	decision.PolicyDigest = c.identity().Digest
	if decision.Status == "would_exclude" && c.Mode == "enforce" {
		decision.Status = "excluded"
	}
	return &decision, nil
}

func (s *postgresDataStore) annotateUtilityHorizons(ctx context.Context, records []Record, purpose string) error {
	c, err := configuredUtilityHorizon()
	if err != nil || c == nil {
		return err
	}
	for i := range records {
		r := &records[i]
		if !c.Policy.TransientKinds[r.Kind] {
			continue
		}
		version := r.Version
		if version == nil {
			version = r.observedVersion
		}
		d, err := s.utilityHorizonDecision(ctx, r.ID, version, purpose, true)
		if err != nil {
			return err
		}
		if c.Mode == "enforce" && purpose == "current" && d.WouldExclude {
			return errors.New("memory: utility horizon changed during selection")
		}
		r.UtilityHorizon = d
		captureRankingCandidate(ctx, *r, "candidate")
	}
	return nil
}
