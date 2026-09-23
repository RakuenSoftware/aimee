package memory

import (
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// The native host supplies its remaining byte allocation. Go owns mandatory
// content, whole-row retention, rendering and exact retained reminder identity.
func nativeRecallLimit(args commandArgs) (*int, error) {
	return commandByteLimit(args, "native_context_bytes")
}

func commandByteLimit(args commandArgs, name string) (*int, error) {
	raw, present := args[name]
	if !present {
		return nil, nil
	}
	var limit *int
	if json.Unmarshal(raw, &limit) != nil || limit == nil || *limit < 0 || *limit > maxDataBody {
		return nil, fmt.Errorf("%s must be an integer between zero and memory message capacity", name)
	}
	return limit, nil
}

type nativeRecallProjection struct {
	Version         int                  `json:"schema_version"`
	Text            string               `json:"text"`
	Bytes           int                  `json:"rendered_bytes"`
	Limit           int                  `json:"max_context_bytes"`
	Digest          string               `json:"digest"`
	SourceDigest    string               `json:"source_digest"`
	Reminders       []string             `json:"retained_reminder_ids"`
	Sources         []typedProjectionRef `json:"retained_items"`
	SelectionDigest string               `json:"selection_digest"`
}

// Render complete rows once. A prefix preserves the existing six-section order;
// the complete hard-rule set is reserved before any optional row is considered.
func projectNativeRecall(b recallBundle, limit int) (nativeRecallProjection, int, error) {
	p := nativeRecallProjection{Version: 1, Limit: limit, Reminders: []string{}, Sources: []typedProjectionRef{}}
	var out strings.Builder
	if len(b.AlwaysOnRules) > 0 {
		rules, err := json.Marshal(b.AlwaysOnRules)
		if err != nil {
			return p, 0, err
		}
		out.WriteString("# Recall\n## Always-on Rules\n")
		out.Write(rules)
		out.WriteString("\n\n")
		if out.Len() > limit {
			return p, 0, protectedRecallOverflow()
		}
	}
	count, stopped := 0, false
	section := func(name string, rows int, row func(int) (string, string, int64, *typedProjectionRef)) {
		if stopped {
			return
		}
		for i := 0; i < rows; i++ {
			text, key, reminder, source := row(i)
			var line strings.Builder
			if out.Len() == 0 {
				line.WriteString("# Recall\n")
			}
			if i == 0 {
				line.WriteString("## " + name + "\n")
			}
			line.WriteString("- " + text)
			if key != "" {
				line.WriteString(" — " + key)
			}
			line.WriteByte('\n')
			// Reserve the terminal separator, including for an exact-fit last row.
			if out.Len()+line.Len()+1 > limit {
				stopped = true
				return
			}
			out.WriteString(line.String())
			count++
			if source != nil {
				p.Sources = append(p.Sources, *source)
			}
			if reminder > 0 {
				p.Reminders = append(p.Reminders, strconv.FormatInt(reminder, 10))
			}
		}
	}
	records := func(name, channel string, rows []RecallRecord) {
		section(name, len(rows), func(i int) (string, string, int64, *typedProjectionRef) {
			row := rows[i]
			var source *typedProjectionRef
			if row.Version != nil {
				kind := ""
				switch row.Store {
				case "kb":
					kind = "memory_record"
				case "user":
					kind = "user_memory_record"
				}
				source = &typedProjectionRef{Channel: channel, ID: strconv.FormatInt(row.ID, 10), Source: &typedSourceVersion{Kind: kind, Version: *row.Version, MemoryParentState: "observed"}}
			}
			return row.Text, row.Key, 0, source
		})
	}
	records("Identity", "native_identity", b.Identity)
	records("Preferences", "native_preferences", b.Preferences)
	records("Active Context", "native_active_context", b.ActiveContext)
	records("Open Commitments", "native_open_commitments", b.OpenCommitments)
	section("Reminders", len(b.Reminders), func(i int) (string, string, int64, *typedProjectionRef) {
		return b.Reminders[i].Text, b.Reminders[i].Key, b.Reminders[i].MemoryID, nil
	})
	section("Directives", len(b.Directives), func(i int) (string, string, int64, *typedProjectionRef) {
		return b.Directives[i].Text, b.Directives[i].Key, 0, nil
	})
	if count > 0 {
		out.WriteByte('\n')
	}
	for _, source := range p.Sources {
		if !validTypedSource(source) {
			return p, 0, fmt.Errorf("invalid native recall source version")
		}
	}
	reminders, sources := p.Reminders, p.Sources
	p = nativeProjectionForText(out.String(), limit)
	p.Reminders, p.Sources = reminders, sources
	p.SelectionDigest = releaseDigest(sources)
	return p, count, nil
}

func nativeProjectionForText(text string, limit int) nativeRecallProjection {
	return nativeRecallProjection{Version: 1, Text: text, Bytes: len(text), Limit: limit,
		Digest: fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(text))), Reminders: []string{}, Sources: []typedProjectionRef{}, SelectionDigest: releaseDigest([]typedProjectionRef{})}
}

func nativeRecallEnvelope(raw []byte, args commandArgs) ([]byte, error) {
	limit, err := nativeRecallLimit(args)
	if err != nil || limit == nil {
		return raw, err
	}
	var envelope map[string]json.RawMessage
	if !utf8.Valid(raw) || len(raw) > maxDataBody || json.Unmarshal(raw, &envelope) != nil || envelope == nil {
		return nil, fmt.Errorf("invalid recall envelope")
	}
	var status string
	if json.Unmarshal(envelope["status"], &status) != nil || status == "" {
		return nil, fmt.Errorf("recall status missing")
	}
	if status != "ok" {
		return raw, nil
	}
	var b recallBundle
	if json.Unmarshal(envelope["recall"], &b) != nil || b.AlwaysOnRules == nil || b.Identity == nil || b.Preferences == nil || b.ActiveContext == nil || b.OpenCommitments == nil || b.Reminders == nil || b.Directives == nil {
		return nil, fmt.Errorf("incomplete recall bundle")
	}
	projection, count, err := projectNativeRecall(b, *limit)
	if err != nil {
		return nil, err
	}
	projection.SourceDigest = fmt.Sprintf("sha256:%x", sha256.Sum256(envelope["recall"]))
	_, selected, err := b.encodePrefix(count)
	if err != nil {
		return nil, err
	}
	envelope["recall"] = selected // activation consumers see only the retained rows
	envelope["native_context"], err = json.Marshal(projection)
	if err != nil {
		return nil, err
	}
	return json.Marshal(envelope)
}

func nativeRecallText(raw []byte, args commandArgs) ([]byte, bus.ModuleStatus) {
	projected, err := nativeRecallEnvelope(raw, args)
	if err != nil {
		kind := "invalid_projection"
		var refusal *contextBudgetError
		if errors.As(err, &refusal) {
			kind = refusal.kind
		}
		projected, _ = json.Marshal(commandError(kind, err.Error()))
	}
	return commandResult(map[string]any{"status": "ok", "json": string(projected)})
}

// The host calls this only after accepting the Go projection's exact bytes.
// Binding source metadata to an opaque request handle remains Go-owned.
func handleNativeSourceRelease(state *sourceReleaseState, args commandArgs) ([]byte, bus.ModuleStatus) {
	var p nativeRecallProjection
	if json.Unmarshal(args["native_projection"], &p) != nil || p.Version != 1 || p.Sources == nil || p.Bytes != len(p.Text) || p.Bytes > p.Limit || p.Limit < 0 || p.Limit > maxDataBody || p.Digest != fmt.Sprintf("sha256:%x", sha256.Sum256([]byte(p.Text))) || p.SelectionDigest != releaseDigest(p.Sources) {
		return commandResult(commandError("invalid_projection", "native source commitment unavailable"))
	}
	for _, ref := range p.Sources {
		if ref.Source == nil || !validTypedSource(ref) || !strings.HasPrefix(ref.Channel, "native_") {
			return commandResult(commandError("invalid_projection", "native source contract unavailable"))
		}
	}
	ticket, err := state.prepare(args, map[string]any{"native_projection": map[string]any{"retained_items": p.Sources}})
	if err != nil {
		return commandResult(commandError("unavailable", "native source release unavailable"))
	}
	return commandResult(map[string]any{"status": "ok", "source_release_ticket": ticket})
}
