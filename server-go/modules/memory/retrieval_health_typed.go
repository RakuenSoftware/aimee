package memory

import (
	"encoding/json"
	"math"
)

// These scores describe the actual assertion fusion algorithm. FusedScore is
// the whole candidate score, not this arm's contribution. Neither score is a
// correctness or trust label.
type healthArm struct {
	Name         string  `json:"name"`
	Rank         int     `json:"rank"`
	RawScore     float64 `json:"raw_score"`
	Contribution float64 `json:"contribution"`
	FusedScore   float64 `json:"fused_score"`
	Policy       string  `json:"policy"`
}

func validatedHealthArms(arms []healthArm) []healthArm {
	if len(arms) == 0 || len(arms) > 3 {
		return nil
	}
	seen := map[string]bool{}
	total := 0.0
	for _, arm := range arms {
		if arm.Name != "lexical" && arm.Name != "vector" && arm.Name != "semantic_graph" {
			return nil
		}
		if seen[arm.Name] || arm.Rank < 1 || arm.Rank > maxDataBody || arm.Policy != "assertion_rrf_k60_v1" || math.IsNaN(arm.RawScore) || math.IsInf(arm.RawScore, 0) || arm.Contribution != assertionRRFContribution(arm.Rank) {
			return nil
		}
		seen[arm.Name] = true
		total += arm.Contribution
	}
	for _, arm := range arms {
		if math.IsNaN(arm.FusedScore) || math.IsInf(arm.FusedScore, 0) || math.Abs(arm.FusedScore-total) > 1e-12 {
			return nil
		}
	}
	return append([]healthArm(nil), arms...)
}

// Called after ingress repacking, using only final selected, version-checked
// owner values. No query, assertion text, evidence span or raw parent IDs enter
// health metadata. A high confidence/authority score never proves low trust
// false or lifecycle safety at release.
func typedHealthRecords(projection *typedContextResult) []healthRecord {
	if projection == nil {
		return nil
	}
	records := []healthRecord{}
	for _, channel := range typedChannelOrder {
		c := projection.Channels[channel]
		if c == nil {
			continue
		}
		for _, item := range c.selected {
			if item.source == nil || item.source.Kind != "semantic_assertion" {
				continue
			}
			raw, err := json.Marshal(item.value)
			ref := typedProjectionRef{Channel: channel, ID: item.id, Source: item.source}
			if err != nil || !validTypedSourceItem(ref, raw) {
				continue
			}
			var hit assertionHit
			if json.Unmarshal(raw, &hit) != nil || !healthLabelName(hit.Kind) || hit.Kind == "unknown" {
				continue
			}
			record := healthRecord{RecordID: healthSourceIdentity(item.source), VersionID: item.source.Version.RecordRevision, Kind: hit.Kind}
			if healthLabelName(hit.Lifecycle) {
				record.State = hit.Lifecycle
			}
			if validHealthConfidenceClass(hit.ConfidenceClass) {
				record.ConfidenceClass = hit.ConfidenceClass
			}
			if assertionTimestamp(hit.ValidFrom) && assertionTimestamp(hit.ValidUntil) {
				record.ValidFrom, record.ValidUntil = hit.ValidFrom, hit.ValidUntil
			}
			for _, trace := range hit.Retrieval {
				if len(hit.Retrieval) > 3 {
					break
				}
				record.Arms = append(record.Arms, healthArm{Name: trace.Channel, Rank: trace.Rank, RawScore: trace.Raw, Contribution: assertionRRFContribution(trace.Rank), FusedScore: trace.Fused, Policy: "assertion_rrf_k60_v1"})
			}
			record.Arms = validatedHealthArms(record.Arms)
			records = append(records, record)
		}
	}
	return records
}

func validHealthConfidenceClass(value string) bool {
	return value == "A" || value == "B" || value == "C"
}
