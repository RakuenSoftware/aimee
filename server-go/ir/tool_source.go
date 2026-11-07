package ir

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"strings"
	"unicode/utf8"
)

// ToolSource is opaque host provenance, never provider input or memory policy.
type ToolSource struct {
	SchemaVersion int    `json:"schema_version"`
	Owner         string `json:"owner"`
	VisibleSHA256 string `json:"visible_sha256"`
	Receipt       string `json:"receipt"`
}

func ValidToolSource(raw, content json.RawMessage) bool {
	var fields map[string]json.RawMessage
	var source ToolSource
	var visible string
	if !Decode(raw, &fields) || len(fields) != 4 || !Decode(raw, &source) || !Decode(content, &visible) || source.SchemaVersion != 1 || len(source.Owner) != 36 || source.Owner == "00000000-0000-0000-0000-000000000000" || len(source.VisibleSHA256) != 64 || strings.Trim(source.VisibleSHA256, "0123456789abcdef") != "" || len(source.Receipt) == 0 || len(source.Receipt) > 524288 || !utf8.ValidString(source.Receipt) || !ValidJSON([]byte(source.Receipt)) {
		return false
	}
	for i, c := range source.Owner {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
		} else if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	digest := sha256.Sum256([]byte(visible))
	return source.VisibleSHA256 == hex.EncodeToString(digest[:])
}
func MessageHasToolSources(raw json.RawMessage) bool {
	var message Message
	if !Decode(raw, &message) {
		return false
	}
	for _, block := range message.Blocks {
		if block.ToolSource != nil {
			return true
		}
	}
	return false
}
func EventHasToolSources(raw json.RawMessage) bool {
	var event struct {
		Kind    string          `json:"kind"`
		Payload json.RawMessage `json:"payload"`
	}
	if json.Unmarshal(raw, &event) != nil {
		return false
	}
	switch event.Kind {
	case "message":
		return MessageHasToolSources(event.Payload)
	case "history":
		var history HistoryEvent
		if Decode(event.Payload, &history) {
			return MessageHasToolSources(history.Message)
		}
	}
	return false
}
