package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// Accept integral JSON numbers such as 7, 7.0 and 7e0, while rejecting
// fractional identities and overflow instead of truncating them as C did.
type ingressInteger int64

func (n *ingressInteger) UnmarshalJSON(raw []byte) error {
	if value, err := strconv.ParseInt(string(raw), 10, 64); err == nil {
		*n = ingressInteger(value)
		return nil
	}
	var value *float64
	if err := json.Unmarshal(raw, &value); err != nil || value == nil ||
		math.Trunc(*value) != *value || *value < -9223372036854775808.0 || *value >= 9223372036854775808.0 {
		return fmt.Errorf("expected an integral int64 JSON number")
	}
	*n = ingressInteger(*value)
	return nil
}

type ingressTaskAnchor struct {
	Project    string         `json:"project"`
	FilePath   string         `json:"file_path"`
	Generation ingressInteger `json:"generation"`
	Freshness  string         `json:"freshness"`
}

type ingressTaskCode struct {
	ingressTaskAnchor
	Accepted   bool     `json:"accepted"`
	Confidence float64  `json:"confidence"`
	Provenance []string `json:"provenance"`
	Snippet    string   `json:"snippet"`
	Span       struct {
		Kind      string          `json:"kind"`
		LineStart *ingressInteger `json:"line_start"`
		LineEnd   *ingressInteger `json:"line_end"`
	} `json:"span"`
}

type ingressTaskMemory struct {
	Anchor     ingressTaskAnchor `json:"anchor"`
	Content    string            `json:"content"`
	Confidence float64           `json:"confidence"`
	ID         ingressInteger    `json:"memory_id"`
	Scope      string            `json:"scope"`
	Provenance string            `json:"provenance"`
}

type ingressTaskPacket struct {
	ingressTaskAnchor
	Status        string         `json:"status"`
	Resolved      bool           `json:"resolved"`
	MaxResults    ingressInteger `json:"max_results"`
	MaxTokens     ingressInteger `json:"max_tokens"`
	ItemCount     ingressInteger `json:"item_count"`
	Answerability struct {
		Decision string `json:"decision"`
	} `json:"answerability"`
	Results []ingressTaskCode   `json:"results"`
	Why     []ingressTaskMemory `json:"why"`
}

func (a ingressTaskAnchor) current(project string, generation ingressInteger) bool {
	return a.Project == project && a.Generation == generation && a.Freshness == "current" && a.FilePath != ""
}

// Preserve the native renderer's escaped-byte budget (an entity may cross the
// limit), but never split a UTF-8 rune. The same formatter is used for paths,
// snippets and anchored memory; none can close the surrounding context tag.
func ingressSingleLine(s string, limit int) string {
	var out strings.Builder
	space := false
	for _, r := range s {
		if out.Len() >= limit {
			break
		}
		if r == '\n' || r == '\r' || r == '\t' {
			r = ' '
		}
		if r < 32 || (r == ' ' && space) {
			continue
		}
		switch r {
		case '<':
			out.WriteString("&lt;")
		case '>':
			out.WriteString("&gt;")
		case '&':
			out.WriteString("&amp;")
		default:
			if out.Len()+utf8.RuneLen(r) > limit {
				return out.String()
			}
			out.WriteRune(r)
		}
		space = r == ' '
	}
	return out.String()
}

// Validate the whole packet before rendering a bounded prefix. In particular,
// oversized early entries must not hide a foreign/stale later evidence row.
func ingressTaskContext(raw json.RawMessage, project string) (string, int, float64) {
	var packet ingressTaskPacket
	if project == "" || json.Unmarshal(raw, &packet) != nil || packet.Status != "ok" ||
		packet.Project != project || packet.Generation <= 0 || packet.Freshness != "current" ||
		!packet.Resolved || packet.Answerability.Decision != "answerable" ||
		packet.Results == nil || packet.Why == nil || len(packet.Results) < 1 ||
		len(packet.Results)+len(packet.Why) > 4 || packet.MaxResults != 4 ||
		packet.MaxTokens != 1200 || packet.ItemCount != ingressInteger(len(packet.Results)+len(packet.Why)) {
		return "", 0, 0
	}
	for _, row := range packet.Results {
		if !row.current(project, packet.Generation) || !row.Accepted || row.Confidence <= 0 || row.Confidence > 1 ||
			len(row.Provenance) == 0 || row.Span.LineStart == nil || row.Span.LineEnd == nil {
			return "", 0, 0
		}
		start, end := *row.Span.LineStart, *row.Span.LineEnd
		if (start > 0 && (row.Span.Kind != "line" || end < start)) ||
			(start <= 0 && (row.Span.Kind != "file" || start != 0 || end != 0)) {
			return "", 0, 0
		}
		for _, signal := range row.Provenance {
			switch signal {
			case "code", "graph", "vector", "memory":
			default:
				return "", 0, 0
			}
		}
	}
	for _, row := range packet.Why {
		if !row.Anchor.current(project, packet.Generation) || row.Content == "" || row.Scope != "project" ||
			row.Provenance != "memory" || row.ID <= 0 || row.Confidence <= 0 || row.Confidence > 1 {
			return "", 0, 0
		}
	}
	var block strings.Builder
	fmt.Fprintf(&block, "recommended (task-conditioned code; project=%s; generation=%d):\n", ingressSingleLine(project, 512), packet.Generation)
	kept, top := 0, 0.0
	for _, row := range packet.Results {
		var item strings.Builder
		fmt.Fprintf(&item, "  - %s", ingressSingleLine(row.FilePath, 512))
		if *row.Span.LineStart > 0 {
			fmt.Fprintf(&item, ":%d", *row.Span.LineStart)
		}
		fmt.Fprintf(&item, " [confidence=%.2f; provenance=%s]\n", row.Confidence, strings.Join(row.Provenance, ","))
		if row.Snippet != "" {
			fmt.Fprintf(&item, "    > %s\n", ingressSingleLine(row.Snippet, 480))
		}
		if block.Len()+item.Len() > 4800 {
			break
		}
		block.WriteString(item.String())
		kept++
		top = max(top, row.Confidence)
	}
	for _, row := range packet.Why {
		item := fmt.Sprintf("  - memory[project; confidence=%.2f] anchored to %s: %s\n", row.Confidence,
			ingressSingleLine(row.Anchor.FilePath, 256), ingressSingleLine(row.Content, 320))
		if block.Len()+len(item) > 4800 {
			break
		}
		block.WriteString(item)
		kept++
	}
	if kept == 0 {
		return "", 0, 0
	}
	return block.String(), kept, top
}

func handleIngressTaskPacket(args commandArgs) ([]byte, bus.ModuleStatus) {
	var project *string
	if json.Unmarshal(args["project"], &project) != nil || project == nil || args["packet"] == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	block, count, confidence := ingressTaskContext(args["packet"], *project)
	return commandResult(map[string]any{"status": "ok", "block": block, "item_count": count, "confidence": confidence})
}
