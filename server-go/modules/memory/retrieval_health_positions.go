package memory

import "strings"

// Rank is one-based within an exact final rendered projection. Separate
// projections do not share a retrieval score or an invented global order.
// More than one position preserves duplicate rendering without double-counting
// that record in concentration metrics.
type healthPosition struct {
	Projection string `json:"projection"`
	Digest     string `json:"projection_sha256"`
	Rank       int    `json:"rank"`
}

func (p healthPosition) valid() bool {
	if p.Rank < 1 || p.Rank > maxReleaseSources || !receiptDigestValid(p.Digest) {
		return false
	}
	switch p.Projection {
	case "facts_projection", "typed_projection", "memory_projection", "native_projection":
		return true
	}
	return false
}

// Read the retained ordering before source-fence deduplication. Collection
// dependencies are not rendered rows and never consume a position. This runs
// only for optional telemetry, after the serving selection is already fixed.
func healthAssemblyPositions(assembly map[string]any) map[string][]healthPosition {
	positions := map[string][]healthPosition{}
	// Native rows can be rendered without versioned references. Their renderer
	// records positions directly; its source list cannot establish all row ranks.
	for _, name := range []string{"facts_projection", "typed_projection", "memory_projection"} {
		projection, ok := assembly[name].(map[string]any)
		if !ok {
			continue
		}
		digest, _ := projection["projection_digest"].(string)
		digest = strings.TrimPrefix(digest, "sha256:")
		if !receiptDigestValid(digest) {
			continue
		}
		refs, _ := projection["retained_items"].([]typedProjectionRef)
		rank := 0
		for _, ref := range refs {
			if ref.Source != nil && strings.HasSuffix(ref.Source.Kind, "_collection") {
				continue
			}
			rank++
			if ref.Source == nil {
				continue
			}
			position := healthPosition{Projection: name, Digest: digest, Rank: rank}
			if !position.valid() {
				continue
			}
			key := healthVersionKey(healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision})
			positions[key] = append(positions[key], position)
		}
	}
	return positions
}

func nativeHealthPositions(records []healthRecord, projection nativeRecallProjection) []healthRecord {
	known := map[string]healthRecord{}
	for _, record := range records {
		known[healthVersionKey(record)] = record
	}
	result := []healthRecord{}
	seen := map[string]bool{}
	for _, ref := range projection.Sources {
		if ref.Source == nil {
			continue
		}
		record := healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision, Kind: "unknown"}
		key := healthVersionKey(record)
		if seen[key] {
			continue
		}
		seen[key] = true
		if previous, ok := known[key]; ok {
			record = previous
		}
		for _, rank := range projection.healthPositions[key] {
			position := healthPosition{Projection: "native_projection", Digest: strings.TrimPrefix(projection.Digest, "sha256:"), Rank: rank}
			if position.valid() {
				record.Positions = append(record.Positions, position)
			}
		}
		if len(record.Positions) > 0 || record.Kind != "unknown" {
			result = append(result, record)
		}
	}
	return result
}

func mergeHealthPositions(old, current []healthPosition, replaceNative bool) []healthPosition {
	if len(old)+len(current) > 2*maxReleaseSources {
		return nil
	}
	for _, group := range [][]healthPosition{old, current} {
		for _, position := range group {
			if !position.valid() {
				return nil
			}
		}
	}
	result := []healthPosition{}
	seen := map[healthPosition]bool{}
	for _, position := range old {
		if position.valid() && !(replaceNative && position.Projection == "native_projection") && !seen[position] {
			result = append(result, position)
			seen[position] = true
		}
	}
	for _, position := range current {
		if position.valid() && !seen[position] {
			result = append(result, position)
			seen[position] = true
		}
	}
	if len(result) > maxReleaseSources {
		return nil // bounded optional evidence; the importer keeps the rank gap
	}
	return result
}
