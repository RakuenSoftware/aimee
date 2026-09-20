package memory

import (
	"bytes"
	"encoding/json"
	"errors"
	"strconv"
)

// MemoryRecordVersion identifies one observed owner/row/revision. It is a
// mutation precondition, not a proof of retrieval eligibility or consumer progress.
// Decimal strings preserve exact identifiers through native JSON transports.
type MemoryRecordVersion struct {
	SchemaVersion  int    `json:"schema_version"`
	OwnerID        string `json:"owner_id"`
	RecordID       string `json:"record_id"`
	RecordRevision string `json:"record_revision"`
}

func (v *MemoryRecordVersion) UnmarshalJSON(raw []byte) error {
	type plain MemoryRecordVersion
	var out plain
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&out); err != nil {
		return err
	}
	*v = MemoryRecordVersion(out)
	return nil
}

func (v *MemoryRecordVersion) validFor(id int64) bool {
	if v == nil || v.SchemaVersion != 1 || len(v.OwnerID) != 36 || v.RecordID != strconv.FormatInt(id, 10) || id <= 0 {
		return false
	}
	for i, c := range v.OwnerID {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
			continue
		}
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	revision, err := strconv.ParseInt(v.RecordRevision, 10, 64)
	return err == nil && revision > 0 && strconv.FormatInt(revision, 10) == v.RecordRevision
}

var errMutationVersionConflict = errors.New("memory: expected version is no longer current; read the current record before correcting it")

func commandExpectedVersion(args commandArgs, id int64) (*MemoryRecordVersion, bool) {
	raw, exists := args["expected_version"]
	if !exists {
		return nil, true
	}
	var version *MemoryRecordVersion
	if json.Unmarshal(raw, &version) != nil || !version.validFor(id) {
		return nil, false
	}
	return version, true
}
