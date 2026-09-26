package memory

import (
	"encoding/json"
	"math"
)

// Native ranking has ordered stages, not additive bonuses across stages.
// Preserve their observed semantics; never translate them to assertion RRF.
func validatedHealthRanking(steps []rankingStep) []rankingStep {
	if len(steps) == 0 || len(steps) > 8 {
		return nil
	}
	finite := func(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }
	for _, step := range steps {
		if !healthLabelName(step.Operation) || !finite(step.Score) || len(step.Contributions) == 0 || len(step.Contributions) > 8 {
			return nil
		}
		for _, c := range step.Contributions {
			if !healthLabelName(c.Arm) || c.Rank < 0 || c.Rank > maxDataBody || !finite(c.Value) {
				return nil
			}
		}
	}
	return append([]rankingStep(nil), steps...)
}
func validatedHealthSelectionPaths(paths []string) []string {
	if len(paths) > 4 {
		return nil
	}
	seen := map[string]bool{}
	result := []string{}
	for _, path := range paths {
		switch path {
		case "native_identity", "native_preferences", "native_active_context", "native_open_commitments":
		default:
			return nil
		}
		if !seen[path] {
			result = append(result, path)
			seen[path] = true
		}
	}
	return result
}
func healthRecordArmsKnown(record healthRecord) bool {
	if len(record.Arms) > 0 || len(record.RankingSteps) > 0 {
		return true
	}
	if len(record.SelectionPaths) == 0 {
		return false
	}
	for _, path := range record.SelectionPaths {
		if path == "native_active_context" {
			return false
		}
	}
	// Identity/preference/commitment SQL selectors have no numeric fusion arms.
	return true
}
func healthNativeRanking(records []healthRecord, raw json.RawMessage, placement string) []healthRecord {
	kind := ""
	switch placement {
	case "user":
		kind = "user_memory_record"
	case "kb":
		kind = "memory_record"
	default:
		return records
	}
	var capture rankingCapture
	if len(raw) == 0 || len(raw) > 128<<10 || json.Unmarshal(raw, &capture) != nil || capture.SchemaVersion != 1 || len(capture.Candidates) > 256 {
		return records
	}
	byVersion := map[string][]rankingStep{}
	for _, candidate := range capture.Candidates {
		if candidate.Version == nil || candidate.ID != candidate.Version.RecordID {
			continue
		}
		// Owner identity and revision, not the numeric ID alone, join placements.
		byVersion[releaseDigest(candidate.Version)] = validatedHealthRanking(candidate.Steps)
	}
	for i, record := range records {
		var identity []string
		if json.Unmarshal([]byte(record.RecordID), &identity) != nil || len(identity) != 3 || identity[0] != kind {
			continue
		}
		version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: identity[1], RecordID: identity[2], RecordRevision: record.VersionID}
		records[i].RankingSteps = byVersion[releaseDigest(version)]
	}
	return records
}
