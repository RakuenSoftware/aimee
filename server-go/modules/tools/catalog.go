package tools

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"strings"
)

// The same reviewed catalog generates the C tools owner's private table.
//
//go:embed catalog.json
var catalogJSON []byte

const (
	PolicyRead uint32 = 1 << iota
	PolicyWrite
	PolicyFilesystem
	PolicyNetwork
	PolicyExternal
	PolicyExecution
	PolicyControl
)

var policyCategories = map[string]uint32{
	"read": PolicyRead, "write": PolicyWrite, "filesystem": PolicyFilesystem,
	"network": PolicyNetwork, "external": PolicyExternal,
	"execution": PolicyExecution, "control": PolicyControl,
}

type toolMetadata struct {
	Class      uint32
	Primary    uint32
	Categories uint32
}

func loadCatalog(body []byte) (map[string]toolMetadata, error) {
	var rows map[string]struct {
		Class      string   `json:"class"`
		SideEffect string   `json:"side_effect"`
		Categories []string `json:"categories"`
	}
	if err := json.Unmarshal(body, &rows); err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		return nil, fmt.Errorf("empty tool catalog")
	}
	classes := map[string]uint32{"read": ClassRead, "write": ClassWrite, "exec": ClassExec, "control": ClassControl, "remote": ClassRemote}
	result := make(map[string]toolMetadata, len(rows))
	for name, row := range rows {
		value := toolMetadata{Class: classes[row.Class], Primary: policyCategories[row.SideEffect]}
		if name == "" || len(name) > nameMax || value.Class == 0 || value.Primary == 0 {
			return nil, fmt.Errorf("invalid metadata for %q", name)
		}
		for _, label := range row.Categories {
			bit := policyCategories[label]
			if bit == 0 || value.Categories&bit != 0 {
				return nil, fmt.Errorf("invalid categories for %q", name)
			}
			value.Categories |= bit
		}
		if value.Categories&value.Primary == 0 {
			return nil, fmt.Errorf("missing primary for %q", name)
		}
		result[name] = value
	}
	return result, nil
}

var catalog, catalogError = loadCatalog(catalogJSON)

func metadata(name string) toolMetadata {
	if catalogError != nil {
		return toolMetadata{}
	}
	if value, ok := catalog[name]; ok {
		return value
	}
	if strings.ContainsRune(name, ':') {
		return toolMetadata{Class: ClassRemote, Primary: PolicyExternal, Categories: PolicyExternal | PolicyNetwork}
	}
	return toolMetadata{}
}
