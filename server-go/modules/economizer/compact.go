package economizer

import (
	"fmt"
	"strconv"
	"strings"
)

// Tool-result body shrink — the single algorithm both reduction seams use.
//
// Ported from src/compact.c.
//
// CORRECTION to an earlier note in this file: compact_body does NOT have two
// live callers. The eager per-result seam (agent_compress_tool_result in
// src/server/agent_policy.c) is uncalled dead code — see eager.go — so the only
// live caller is the economizer's lazy lever in context_fold.c. Moving this to
// Go therefore creates no two-languages problem, provided the dead C seam is
// deleted rather than left in place.
//
// Output byte-identity is load-bearing, not cosmetic: these bodies sit inside the
// folded prefix, and a prefix that differs by one byte is a cold prompt cache. So
// the C formatting quirks below are reproduced deliberately rather than
// modernised.

const (
	CompactDefaultThreshold = 4096 // bytes: pass through unchanged below this
	CompactDefaultHeadBytes = 512  // leading bytes kept in plain-text results
	CompactDefaultTailBytes = 1024 // trailing bytes kept in plain-text results
)

// PerToolCompact is a per-tool threshold override. A Threshold of -1 disables
// compaction for that tool entirely.
type PerToolCompact struct {
	Tool      string
	Threshold int
}

// CompactConfig mirrors compact_config_t. A zero value means "use defaults",
// except Enabled, which the caller sets explicitly.
type CompactConfig struct {
	Enabled   bool
	Threshold int // 0 = CompactDefaultThreshold
	HeadBytes int // 0 = CompactDefaultHeadBytes
	TailBytes int // 0 = CompactDefaultTailBytes
	PerTool   []PerToolCompact
}

// cFormatG reproduces C's "%g": 6 significant digits, then trailing zeros and a
// trailing decimal point removed.
//
// Go's %g defaults to the shortest representation that round-trips, which is a
// DIFFERENT string for the same double (C gives 1.23457e-06 where Go gives
// 1.234567e-06). Since this text lands in a cache-sensitive prefix, the C
// spelling is the one that matters.
func cFormatG(v float64) string {
	s := strconv.FormatFloat(v, 'g', 6, 64)
	if !strings.ContainsAny(s, ".") {
		return s
	}
	mant, exp, hasExp := strings.Cut(s, "e")
	if strings.Contains(mant, ".") {
		mant = strings.TrimRight(mant, "0")
		mant = strings.TrimSuffix(mant, ".")
	}
	if hasExp {
		return mant + "e" + exp
	}
	return mant
}

// boundBuf reproduces the C summary buffer's snprintf semantics exactly.
//
// The C code does `n = snprintf(buf + pos, cap - pos, ...); if (n > 0) pos += n`
// — pos advances by the WOULD-BE length even when the write was truncated, and
// callers then test `pos >= cap` to stop. Reproducing that is what keeps
// truncation landing on the same byte as the C version.
type boundBuf struct {
	buf []byte
	pos int
	cap int
}

func (b *boundBuf) appendf(format string, args ...any) {
	s := fmt.Sprintf(format, args...)
	if b.pos < b.cap {
		room := b.cap - b.pos
		w := s
		if len(w) > room-1 { // snprintf reserves one byte for the NUL
			if room-1 < 0 {
				w = ""
			} else {
				w = w[:room-1]
			}
		}
		b.buf = append(b.buf[:min(b.pos, len(b.buf))], w...)
	}
	b.pos += len(s) // the would-be length, exactly as snprintf reports it
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// startsWithJSON reports whether the body's first non-space byte opens an object
// or array. ASCII whitespace only, matching the C isspace on the C locale.
func startsWithJSON(s string) bool {
	i := 0
	for i < len(s) && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\v' || s[i] == '\f' || s[i] == '\r') {
		i++
	}
	return i < len(s) && (s[i] == '{' || s[i] == '[')
}

// describeJSON renders the structural summary, depth-limited so a deeply nested
// document cannot produce an enormous summary.
func describeJSON(node *JSONValue, b *boundBuf, depth int) {
	if node == nil || b.pos >= b.cap {
		return
	}
	switch node.Kind {
	case JSONObject:
		count := len(node.Keys)
		b.appendf("{")
		first := true
		for i, key := range node.Keys {
			if b.pos >= b.cap {
				break
			}
			if !first {
				b.appendf(", ")
			}
			first = false
			b.appendf("\"%s\": ", key)
			if depth < 2 {
				describeJSON(node.Vals[i], b, depth+1)
			} else {
				b.appendf("%s", typeGlyph(node.Vals[i]))
			}
		}
		b.appendf("}")
		if count > 5 {
			b.appendf(" /* %d keys */", count)
		}
	case JSONArray:
		count := len(node.Items)
		if count == 0 {
			b.appendf("[]")
			return
		}
		b.appendf("[/* %d items */", count)
		if depth < 2 {
			b.appendf(" ")
			describeJSON(node.Items[0], b, depth+1)
			if count > 1 {
				b.appendf(", ...")
			}
		}
		b.appendf("]")
	case JSONString:
		if len(node.Str) <= 64 {
			b.appendf("\"%s\"", node.Str)
		} else {
			b.appendf("\"%s...\"", node.Str[:60]) // C's %.60s is a BYTE precision
		}
	case JSONNumber:
		b.appendf("%s", cFormatG(node.Num))
	case JSONTrue:
		b.appendf("true")
	case JSONFalse:
		b.appendf("false")
	default:
		b.appendf("null")
	}
}

func typeGlyph(n *JSONValue) string {
	if n == nil {
		return "null"
	}
	switch n.Kind {
	case JSONString:
		return "<string>"
	case JSONNumber:
		return "<number>"
	case JSONObject:
		return "{...}"
	case JSONArray:
		return "[...]"
	case JSONTrue, JSONFalse:
		return "<bool>"
	}
	return "null"
}

// compactJSONSummary returns the structural summary, or ok=false when the body
// is not valid JSON (the caller then falls back to head+tail).
func compactJSONSummary(raw string) (string, bool) {
	node := ParseJSON(raw)
	if node == nil {
		return "", false
	}
	const summaryCap = 2048
	b := &boundBuf{buf: make([]byte, 0, summaryCap), cap: summaryCap - 64}
	b.appendf("[compacted JSON summary]\n")
	describeJSON(node, b, 0)

	out := string(b.buf)
	// The size hint is written with the FULL buffer as its bound, not the
	// reduced describe cap, matching the C snprintf against sizeof(summary).
	out += fmt.Sprintf("\n[original: %d bytes]", len(raw))
	if len(out) > summaryCap-1 {
		out = out[:summaryCap-1]
	}
	return out, true
}

// compactPlaintext keeps head+tail bytes with a truncation notice between.
func compactPlaintext(raw string, headBytes, tailBytes int) string {
	head, tail := headBytes, tailBytes
	if head+tail >= len(raw) {
		return raw
	}
	omitted := len(raw) - head - tail
	notice := fmt.Sprintf("\n[... %d bytes omitted ...]\n", omitted)
	return raw[:head] + notice + raw[len(raw)-tail:]
}

// CompactBody applies the three-strategy shrink: pass-through, JSON structural
// summary, then head+tail.
//
// toolName may be empty to skip per-tool overrides — the lazy economizer seam
// passes none, because it operates on deep-copied mixed-origin history where a
// single tool identity does not apply.
func CompactBody(raw, toolName string, cfg *CompactConfig) string {
	if raw == "" {
		return ""
	}

	enabled := true
	threshold := CompactDefaultThreshold
	headBytes := CompactDefaultHeadBytes
	tailBytes := CompactDefaultTailBytes

	if cfg != nil {
		enabled = cfg.Enabled
		if cfg.Threshold > 0 {
			threshold = cfg.Threshold
		}
		if cfg.HeadBytes > 0 {
			headBytes = cfg.HeadBytes
		}
		if cfg.TailBytes > 0 {
			tailBytes = cfg.TailBytes
		}
		if toolName != "" {
			for _, pt := range cfg.PerTool {
				if pt.Tool == toolName {
					if pt.Threshold == -1 {
						return raw // compaction disabled for this tool
					}
					threshold = pt.Threshold
					break
				}
			}
		}
	}

	if !enabled {
		return raw
	}
	if threshold < 64 {
		threshold = 64 // floor: never compact below 64 bytes
	}
	if len(raw) <= threshold {
		return raw
	}

	if startsWithJSON(raw) {
		if summary, ok := compactJSONSummary(raw); ok {
			return summary
		}
	}
	return compactPlaintext(raw, headBytes, tailBytes)
}
