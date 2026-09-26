package memory

import "encoding/json"

func healthSourceIdentity(source *typedSourceVersion) string {
	if source == nil {
		return ""
	}
	raw, _ := json.Marshal([]string{source.Kind, source.Version.OwnerID, source.Version.RecordID})
	return string(raw)
}
func healthVersionKey(record healthRecord) string {
	raw, _ := json.Marshal([]string{record.RecordID, record.VersionID})
	return string(raw)
}

// Record metadata comes from the same versioned Go recall snapshot that was
// actually retained. Channel names are not substituted for memory kinds, and
// common parent IDs never become invented independent source families.
func nativeHealthRecords(bundle recallBundle, selected []typedProjectionRef) []healthRecord {
	observed := map[string]healthRecord{}
	for section, rows := range [][]RecallRecord{bundle.Identity, bundle.Preferences, bundle.ActiveContext, bundle.OpenCommitments} {
		for _, row := range rows {
			if row.Version == nil || row.Kind == "" {
				continue
			}
			kind := ""
			if row.Store == "user" {
				kind = "user_memory_record"
			} else if row.Store == "kb" {
				kind = "memory_record"
			} else {
				continue
			}
			source := &typedSourceVersion{Kind: kind, Version: *row.Version}
			record := healthRecord{RecordID: healthSourceIdentity(source), VersionID: row.Version.RecordRevision, Kind: row.Kind, Historical: row.Historical}
			if row.Authorship != nil {
				if validHealthProvenance(row.Authorship.Category) {
					record.Provenance = row.Authorship.Category
				}
				switch row.Authorship.Category {
				case "agent_message":
					value := true
					record.LowTrust = &value
				case "user_stated":
					value := false
					record.LowTrust = &value
				}
			}
			record.SelectionPaths = append(observed[healthVersionKey(record)].SelectionPaths, []string{"native_identity", "native_preferences", "native_active_context", "native_open_commitments"}[section])
			observed[healthVersionKey(record)] = record
		}
	}
	result := []healthRecord{}
	seen := map[string]bool{}
	for _, ref := range selected {
		if ref.Source == nil {
			continue
		}
		key := healthVersionKey(healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision})
		if record, ok := observed[key]; ok && !seen[key] {
			result = append(result, record)
			seen[key] = true
		}
	}
	return result
}

// Merge only metadata belonging to final retained versions. Refreshing a native
// projection cannot keep diagnostics for rows that left the model input.
func mergeHealthSelectionMetadata(prior json.RawMessage, assembly map[string]any, selected []typedProjectionRef) []healthRecord {
	var previous struct {
		Records []healthRecord `json:"health_records"`
	}
	_ = json.Unmarshal(prior, &previous)
	_, native := assembly["native_projection"]
	appendNative, _ := assembly["append_native_sources"].(bool)
	records := map[string]healthRecord{}
	for _, record := range previous.Records {
		record.Positions = mergeHealthPositions(record.Positions, nil, native && !appendNative)
		records[healthVersionKey(record)] = record
	}
	if current, ok := assembly["health_records"].([]healthRecord); ok {
		for _, record := range current {
			record.Positions = mergeHealthPositions(records[healthVersionKey(record)].Positions, record.Positions, false)
			records[healthVersionKey(record)] = record
		}
	}
	positions := healthAssemblyPositions(assembly)
	var result []healthRecord
	for _, ref := range selected {
		if ref.Source == nil {
			continue
		}
		key := healthVersionKey(healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision})
		record, ok := records[key]
		if !ok && len(positions[key]) > 0 {
			record = healthRecord{RecordID: healthSourceIdentity(ref.Source), VersionID: ref.Source.Version.RecordRevision, Kind: "unknown"}
			ok = true
		}
		if ok && len(record.Kind) <= 64 {
			record.Positions = mergeHealthPositions(record.Positions, positions[key], false)
			result = append(result, record)
			delete(records, key)
			delete(positions, key)
		}
	}
	return result
}

// Optional observations neither change the required assembly commitment nor
// consume its bounded handle pool. They have a separately bounded allowance.
func healthIndependentAssemblyDigest(assembly map[string]any) string {
	filtered := assembly
	for _, key := range []string{"health_context", "health_records", "health_labels"} {
		if _, exists := assembly[key]; exists {
			filtered = make(map[string]any, len(assembly))
			for k, value := range assembly {
				filtered[k] = value
			}
			delete(filtered, "health_context")
			delete(filtered, "health_records")
			delete(filtered, "health_labels")
			break
		}
	}
	return releaseDigest(filtered)
}
func receiptMetadataWithHealth(entry *sourceReleaseEntry) json.RawMessage {
	if len(entry.healthMetadata) == 0 {
		return entry.assemblyMetadata
	}
	var base, optional map[string]json.RawMessage
	if json.Unmarshal(entry.assemblyMetadata, &base) != nil || base == nil || json.Unmarshal(entry.healthMetadata, &optional) != nil {
		return entry.assemblyMetadata
	}
	for _, key := range []string{"health_context", "health_records", "health_labels"} {
		if value := optional[key]; len(value) > 0 {
			base[key] = value
		}
	}
	result, err := json.Marshal(base)
	if err != nil || len(result) > 12000 {
		return entry.assemblyMetadata
	}
	return result
}
