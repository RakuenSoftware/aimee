package families

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"sort"
	"strings"
	"unicode/utf8"
)

const opSessionToolSourceHistorySave uint32 = 55

type sourceHistoryPair struct {
	key      [32]byte
	body     [32]byte
	source   *canonical.ToolSource
	id       string
	unpaired bool
}

func sourceHistoryDigest(input any) ([32]byte, bool) {
	raw, err := json.Marshal(input)
	if err != nil {
		return [32]byte{}, false
	}
	var value any
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	if decoder.Decode(&value) != nil {
		return [32]byte{}, false
	}
	raw, err = json.Marshal(value)
	if err != nil {
		return [32]byte{}, false
	}
	return sha256.Sum256(raw), true
}

// Match the visible provider projection, not the JSON type wrapper. The actual
// C path unwraps strings and compact-prints structured values. Raw compaction
// preserves wide numeric bytes instead of decoding them through float64.
func sourceHistoryVisibleDigest(content json.RawMessage) ([32]byte, bool) {
	var visible string
	if canonical.Decode(content, &visible) {
		return sha256.Sum256([]byte(visible)), true
	}
	var compact bytes.Buffer
	if !canonical.ValidJSON(content) || json.Compact(&compact, content) != nil {
		return [32]byte{}, false
	}
	return sha256.Sum256(compact.Bytes()), true
}

// Inputs/results remain exact typed JSON, including wide numeric RawMessages.
// The lookup is O(n log n), bounded by the canonical history wire/page budgets.
func sourceHistoryPairs(messages []canonical.Message, relevant map[string]bool) ([]sourceHistoryPair, bool) {
	pending := map[string]canonical.Block{}
	pairs := []sourceHistoryPair{}
	for _, message := range messages {
		for _, block := range message.Blocks {
			if !relevant[block.ToolID] {
				continue
			}
			switch block.Type {
			case "tool_use":
				if _, exists := pending[block.ToolID]; exists {
					return nil, false
				}
				pending[block.ToolID] = block
			case "tool_result":
				call, exists := pending[block.ToolID]
				if !exists {
					if block.ToolSource != nil {
						return nil, false
					}
				}
				delete(pending, block.ToolID)
				source := block.ToolSource
				block.ToolSource = nil
				// Classification/cache presentation metadata may be added by the local
				// host after the immutable event. It is not native source authority.
				// Bind the exact semantic call/result identity and receipt instead.
				identity := struct {
					Call struct {
						ID        string          `json:"id"`
						Name      string          `json:"name"`
						Namespace string          `json:"namespace"`
						Input     json.RawMessage `json:"input"`
					} `json:"call"`
					Result struct {
						ID      string          `json:"id"`
						Content json.RawMessage `json:"content"`
						IsError *bool           `json:"is_error"`
					} `json:"result"`
				}{}
				identity.Call.ID = call.ToolID
				identity.Call.Name = call.Name
				identity.Call.Namespace = call.Namespace
				identity.Call.Input = call.Input
				identity.Result.ID = block.ToolID
				identity.Result.Content = block.Content
				identity.Result.IsError = block.IsError
				key, ok := sourceHistoryDigest(identity)
				if !ok {
					return nil, false
				}
				body, ok := sourceHistoryVisibleDigest(block.Content)
				if !ok {
					return nil, false
				}
				pairs = append(pairs, sourceHistoryPair{key: key, body: body, source: source, id: block.ToolID, unpaired: !exists})
			}
		}
	}
	return pairs, true
}
func toolSourceHistoryPreserved(current, next []canonical.Message) bool {
	relevant := map[string]bool{}
	sourceIDs := map[string]bool{}
	sourceBodies := map[[32]byte]bool{}
	anyReceipt := false
	for _, message := range current {
		for _, block := range message.Blocks {
			if block.ToolSource != nil {
				anyReceipt = true
				relevant[block.ToolID] = true
				sourceIDs[block.ToolID] = true
				body, ok := sourceHistoryVisibleDigest(block.Content)
				if !ok {
					return false
				}
				sourceBodies[body] = true
			}
		}
	}
	for _, message := range next {
		for _, block := range message.Blocks {
			if block.ToolSource != nil {
				anyReceipt = true
				relevant[block.ToolID] = true
			}
			if block.Type == "tool_result" && len(sourceBodies) > 0 {
				body, ok := sourceHistoryVisibleDigest(block.Content)
				if !ok {
					return false
				}
				if sourceBodies[body] {
					relevant[block.ToolID] = true
				}
			}
		}
	}
	// General IR validation remains with DecodeMessages. Without native source
	// custody, this operation imposes no extra constraints on ordinary history.
	if !anyReceipt {
		return true
	}

	original, ok := sourceHistoryPairs(current, relevant)
	if !ok {
		return false
	}
	retained, ok := sourceHistoryPairs(next, relevant)
	if !ok {
		return false
	}
	indices := map[[32]byte][]int{}
	for i, p := range original {
		indices[p.key] = append(indices[p.key], i)
		if p.source != nil {
			sourceIDs[p.id] = true
		}
	}
	cursor := -1
	for _, p := range retained {
		if p.unpaired && (sourceIDs[p.id] || sourceBodies[p.body]) {
			return false
		}
		list := indices[p.key]
		position := sort.Search(len(list), func(i int) bool { return list[i] > cursor })
		if position == len(list) {
			if p.source != nil || sourceIDs[p.id] || sourceBodies[p.body] {
				return false
			}
			continue
		}
		at := list[position]
		expected := original[at].source
		if (expected == nil) != (p.source == nil) {
			return false
		}
		if expected != nil && *expected != *p.source {
			return false
		}
		cursor = at
	}
	return true
}

// Actual Core0 custody may preserve only source receipts already committed to
// the same locked session/actor and running turn. It cannot mint source authority.
func sessionToolSourceHistorySave(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 6 {
		return store.StatusInvalid, nil, nil
	}
	for _, id := range f[:5] {
		if id == "" || len(id) > 1024 || !utf8.ValidString(id) || strings.ContainsRune(id, 0) {
			return store.StatusInvalid, nil, nil
		}
	}
	next, err := canonical.DecodeMessages([]byte(f[5]))
	if err != nil {
		return store.StatusInvalid, nil, nil
	}
	var actor string
	if err := q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&actor); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if actor != f[3] {
		return store.StatusInvalid, nil, nil
	}
	status, cells, err := sessionIRRunningHistoryLoad(ctx, q, []string{f[0], f[3], f[4]})
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	if len(cells) != 1 {
		return store.StatusInvalid, nil, nil
	}
	current, err := canonical.DecodeMessages([]byte(cells[0]))
	if err != nil || !toolSourceHistoryPreserved(current, next) {
		return store.StatusInvalid, nil, nil
	}
	return sessionIRHistorySaveAuthorized(ctx, q, []string{f[0], f[1], f[2], f[3], f[5]}, true)
}
