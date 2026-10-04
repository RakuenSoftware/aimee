package memory

import (
	"encoding/json"
	"errors"

	memorycontract "github.com/JBailes/aimee/server-go/memory"
)

type MemoryRecordVersion = memorycontract.MemoryRecordVersion

var errMutationVersionConflict = errors.New("memory: expected version is no longer current; read the current record before correcting it")

func commandExpectedVersion(args commandArgs, id int64) (*MemoryRecordVersion, bool) {
	return commandRecordVersion(args, "expected_version", id)
}

func commandRecordVersion(args commandArgs, name string, id int64) (*MemoryRecordVersion, bool) {
	raw, exists := args[name]
	if !exists {
		return nil, true
	}
	var version *MemoryRecordVersion
	if json.Unmarshal(raw, &version) != nil || !version.ValidFor(id) {
		return nil, false
	}
	return version, true
}
