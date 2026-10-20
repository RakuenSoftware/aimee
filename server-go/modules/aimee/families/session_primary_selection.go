package families

import (
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strings"
	"unicode/utf8"
)

const (
	opServerSessionPrimaryGet uint32 = 48
	opServerSessionPrimarySet uint32 = 49
)

func sessionPrimaryFields(f []string, n int) bool {
	if len(f) != n {
		return false
	}
	// Match the actual host principal and session buffers, including their NUL.
	for i, v := range f[:2] {
		bound := 160
		if i == 1 {
			bound = 128
		}
		if v == "" || len(v) >= bound || strings.ContainsRune(v, 0) || !utf8.ValidString(v) {
			return false
		}
	}
	return n == 2 || len(f[2]) < 64 && !strings.ContainsRune(f[2], 0) && utf8.ValidString(f[2])
}
func sessionPrimaryDecode(raw string) (map[string]json.RawMessage, bool) {
	var metadata map[string]json.RawMessage
	if !canonical.Decode([]byte(raw), &metadata) || metadata == nil {
		return nil, false
	}
	return metadata, true
}
func sessionPrimaryGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !sessionPrimaryFields(f, 2) {
		return store.StatusInvalid, nil, nil
	}
	var raw string
	err := q.QueryRow(ctx, `SELECT metadata FROM server_sessions WHERE id=$1 AND principal=$2 FOR SHARE`, f[1], f[0]).Scan(&raw)
	if store.IsNoRows(err) {
		return store.StatusInvalid, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	metadata, ok := sessionPrimaryDecode(raw)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	agent := ""
	if value, present := metadata["primary_agent"]; present {
		if string(value) == "null" || !canonical.Decode(value, &agent) || !sessionPrimaryFields([]string{f[0], f[1], agent}, 3) {
			return store.StatusInvalid, nil, nil
		}
	}
	return store.StatusOK, []string{agent}, nil
}
func sessionPrimarySet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !sessionPrimaryFields(f, 3) {
		return store.StatusInvalid, nil, nil
	}
	var raw string
	err := q.QueryRow(ctx, `SELECT metadata FROM server_sessions WHERE id=$1 AND principal=$2 FOR UPDATE`, f[1], f[0]).Scan(&raw)
	if store.IsNoRows(err) {
		return store.StatusInvalid, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	metadata, ok := sessionPrimaryDecode(raw)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if f[2] == "" {
		delete(metadata, "primary_agent")
	} else {
		metadata["primary_agent"], _ = json.Marshal(f[2])
	}
	encoded, err := json.Marshal(metadata)
	if err != nil {
		return 0, nil, err
	}
	_, err = q.Exec(ctx, `UPDATE server_sessions SET metadata=$3 WHERE id=$1 AND principal=$2`, f[1], f[0], string(encoded))
	return store.StatusOK, nil, err
}
