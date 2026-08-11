package economizer

import (
	"fmt"
	"strings"
)

// Rolling history fold (§1) and boundary-free tool-body compression (§4).
//
// Ported from src/modules/economizer/context_fold.c, including the tail-note
// placement fix from #2552 — see CompressView.

const (
	FoldDefaultRetainedMsgs = 8
	FoldDefaultMinFoldMsgs  = 4
	FoldDefaultExcerptBytes = 160
	FoldDefaultTailCapMsgs  = 24
)

// FoldConfig mirrors fold_config_t.
type FoldConfig struct {
	Enabled               bool
	RetainedMsgs          int  // trailing messages kept at full fidelity (0 -> default)
	MinFoldMsgs           int  // fold only if >= this many would fold (0 -> default)
	ReasoningExcerptBytes int  // per-message excerpt kept in the skeleton (0 -> default)
	RegisterEnabled       bool // annotate folded assistant lines with their register
	Closet                ClosetConfig
	CompactHeadBytes      int
	CompactTailBytes      int
}

// FoldFreeze is the §3 freeze boundary plus the digest that guards it.
//
// The digest is what makes reuse honest: a mid-run mutation of the folded prefix
// (compaction rewriting history, say) would otherwise be reported as a warm
// reuse when the bytes had in fact changed, claiming a cache that is cold.
type FoldFreeze struct {
	Active       bool
	FrozenSplit  int
	TailCapMsgs  int // re-epoch when (count - frozenSplit) exceeds this (0 -> default)
	Epochs       int // count of boundary advances (diagnostic)
	PrefixDigest uint64
}

// FoldResult is the outcome of a fold or compress pass.
type FoldResult struct {
	Messages       *JSONValue // NEW array; nil when nothing changed
	Folded         bool
	FoldedMsgs     int
	RetainedMsgs   int
	ReusedBoundary bool // this fold reused a frozen boundary (cache-warm)
	ClosetEvict    EvictResult
}

// isCleanUserTurn reports whether m is a user turn carrying no tool_result.
//
// The fold may only split on one of these: cutting between a tool_use and its
// tool_result would orphan the pair and every provider rejects that.
func isCleanUserTurn(m *JSONValue) bool {
	if m == nil || m.GetString("role") != "user" {
		return false
	}
	content := m.Get("content")
	if content.IsString() {
		return true
	}
	if content.IsArray() {
		for _, b := range content.Items {
			if b.GetString("type") == "tool_result" {
				return false
			}
		}
		return true
	}
	return false
}

// appendExcerpt appends up to max bytes of s, single-lined, marking truncation
// with an ellipsis.
//
// If the byte cap would land mid-UTF-8-sequence it backs up to a character
// boundary, so the emitted string never contains a split multibyte character.
func appendExcerpt(b *strings.Builder, s string, max int) {
	n := len(s)
	if n > max {
		n = max
		// back off any continuation byte (0x80-0xBF) so [0,n) ends on a
		// complete character
		for n > 0 && s[n]&0xC0 == 0x80 {
			n--
		}
	}
	for i := 0; i < n; i++ {
		c := s[i]
		if c == '\n' || c == '\r' || c == '\t' {
			c = ' '
		}
		b.WriteByte(c)
	}
	if n < len(s) {
		b.WriteString("…")
	}
}

// skeletonPrefix emits the "role: " (or "role/register: ") prefix.
func skeletonPrefix(b *strings.Builder, role, txt string, registerOn bool) {
	if registerOn && role == "assistant" {
		fmt.Fprintf(b, "%s/%s: ", role, ParseRegister(txt).Label())
		return
	}
	if role == "" {
		role = "?"
	}
	fmt.Fprintf(b, "%s: ", role)
}

// skeletonMessage emits one folded message's skeleton line(s) and nominates its
// identifiers into set.
//
// Format-aware and ADDITIVE: a single message can carry multiple shapes at once
// — an OpenAI assistant turn has both a string `content` AND a top-level
// `tool_calls` — so the extractors never early-return, and the closet captures
// identifiers from every provider's calls and results, not just Anthropic's.
func skeletonMessage(b *strings.Builder, m *JSONValue, turn, excerpt int, registerOn bool, set *CoordSet) {
	role := m.GetString("role")
	content := m.Get("content")
	prov := Provenance{Lane: LaneAgent, TurnID: int64(turn), ToolCallID: -1, ResultIndex: -1}

	// (1) String content (OpenAI/Gemini text, or any plain turn).
	if content.IsString() {
		txt := content.Str
		NominateInto(txt, &prov, set)
		skeletonPrefix(b, role, txt, registerOn)
		appendExcerpt(b, txt, excerpt)
		b.WriteByte('\n')
	} else if content.IsArray() {
		// (2) Anthropic content-block array (text / tool_use / tool_result).
		for _, blk := range content.Items {
			switch blk.GetString("type") {
			case "":
				continue
			case "text":
				txt := blk.GetString("text")
				if txt == "" && blk.Get("text") == nil {
					continue
				}
				NominateInto(txt, &prov, set)
				skeletonPrefix(b, role, txt, registerOn)
				appendExcerpt(b, txt, excerpt)
				b.WriteByte('\n')
			case "tool_use":
				name := blk.GetString("name")
				inp := ""
				if input := blk.Get("input"); input != nil {
					inp = PrintJSONUnformatted(input)
					NominateInto(inp, &prov, set)
				}
				if name == "" {
					name = "tool"
				}
				fmt.Fprintf(b, "  $ %s ", name)
				appendExcerpt(b, inp, excerpt)
				b.WriteByte('\n')
			case "tool_result":
				if cv, ok := blk.Get("content").Text(); ok {
					NominateInto(cv, &prov, set)
					b.WriteString("    → ")
					appendExcerpt(b, cv, excerpt)
					fmt.Fprintf(b, " (%d bytes)\n", len(cv))
				}
			}
		}
	}

	// (3) OpenAI / Gemini-as-openai assistant tool calls: a top-level `tool_calls`
	// array whose function.arguments is a JSON STRING.
	if toolCalls := m.Get("tool_calls"); toolCalls.IsArray() {
		for _, tc := range toolCalls.Items {
			fn := tc.Get("function")
			name := fn.GetString("name")
			args := fn.GetString("arguments")
			if args != "" {
				NominateInto(args, &prov, set)
			}
			if name == "" {
				name = "tool"
			}
			fmt.Fprintf(b, "  $ %s ", name)
			appendExcerpt(b, args, excerpt)
			b.WriteByte('\n')
		}
	}

	// (4) Responses (chatgpt) top-level items, mirroring the tool_use /
	// tool_result branches above.
	switch m.GetString("type") {
	case "function_call":
		name := m.GetString("name")
		args := m.GetString("arguments")
		if args != "" {
			NominateInto(args, &prov, set)
		}
		if name == "" {
			name = "tool"
		}
		fmt.Fprintf(b, "  $ %s ", name)
		appendExcerpt(b, args, excerpt)
		b.WriteByte('\n')
	case "function_call_output":
		if ov, ok := m.Get("output").Text(); ok {
			NominateInto(ov, &prov, set)
			b.WriteString("    → ")
			appendExcerpt(b, ov, excerpt)
			fmt.Fprintf(b, " (%d bytes)\n", len(ov))
		}
	}
}

// prefixDigest is FNV-1a over the serialized bytes of messages[0..n), also
// returning their total serialized size.
//
// Detects prefix mutation for freeze reuse and bounds the closet ratio cap in one
// pass.
func prefixDigest(messages *JSONValue, n int) (uint64, int) {
	var h uint64 = 14695981039346656037
	total := 0
	for i := 0; i < n; i++ {
		item := messages.At(i)
		if item == nil {
			continue
		}
		s := PrintJSONUnformatted(item)
		for j := 0; j < len(s); j++ {
			h ^= uint64(s[j])
			h *= 1099511628211
		}
		total += len(s)
	}
	return h, total
}

// compressBodyField shrinks one body in place, returning whether it was replaced
// and how many raw bytes it had.
func compressBodyField(parent *JSONValue, key string, cc *CompactConfig, turn int, set *CoordSet) (bool, int) {
	node := parent.Get(key)
	body, ok := node.Text()
	if !ok {
		return false, 0
	}
	// Below threshold -> keep verbatim.
	if cc.Threshold > 0 && len(body) <= cc.Threshold {
		return false, 0
	}
	out := CompactBody(body, "", cc)
	// Commit only on a genuine net shrink, so an over-threshold-but-tiny body
	// never EXPANDS.
	if len(out) == 0 || len(out) >= len(body) {
		return false, 0
	}
	prov := Provenance{Lane: LaneAgent, TurnID: int64(turn), ToolCallID: -1, ResultIndex: -1}
	NominateInto(body, &prov, set)
	parent.Set(key, NewString(out))
	return true, len(body)
}

// compressMessageBodies compresses every oversized tool-result body carried by
// one message, across all three provider shapes.
//
// The shapes are mutually exclusive per message (an OpenAI tool result is a
// string `content`; an Anthropic tool_result is a block inside an ARRAY
// `content`; a Responses output is a top-level `output`), so no body is
// double-counted.
func compressMessageBodies(m *JSONValue, cc *CompactConfig, turn int, set *CoordSet) (int, int) {
	n, raw := 0, 0
	role := m.GetString("role")
	itype := m.GetString("type")

	// (A) OpenAI / Gemini-as-openai tool result: role=="tool" with string content.
	if role == "tool" {
		if ok, b := compressBodyField(m, "content", cc, turn, set); ok {
			n, raw = n+1, raw+b
		}
	}

	// (B) Anthropic tool_result content-block(s) inside a content ARRAY. A role
	// "tool" message has a STRING content, so this never re-touches it.
	if content := m.Get("content"); content.IsArray() {
		for _, blk := range content.Items {
			if blk.GetString("type") == "tool_result" {
				if ok, b := compressBodyField(blk, "content", cc, turn, set); ok {
					n, raw = n+1, raw+b
				}
			}
		}
	}

	// (C) Responses (chatgpt) top-level function_call_output item.
	if itype == "function_call_output" {
		if ok, b := compressBodyField(m, "output", cc, turn, set); ok {
			n, raw = n+1, raw+b
		}
	}
	return n, raw
}

// CompressView performs boundary-free tool-result BODY compression.
//
// Unlike FoldView this needs NO clean-user-turn boundary: it shrinks oversized
// bodies in place while keeping the carrying message, its role/type and its
// tool_use_id / tool_call_id, so a tool_use and its result are never split. That
// is why it engages on autonomous tool-loops, where the fold's boundary never
// appears.
//
// The conserved-identifier note is APPENDED at the tail, not prepended. That
// placement is load-bearing (#2552): the note summarizes a region that GROWS as
// messages age out of the retained band, so at the head it sat inside the fold's
// frozen prefix and changed the prefix bytes every turn, silently defeating the
// §3 freeze whenever compress and the fold ran together. The tail already varies
// each turn, so a growing note costs nothing there.
func CompressView(messages *JSONValue, cfg *FoldConfig) FoldResult {
	out := FoldResult{ClosetEvict: EvictNone}
	if messages == nil || !messages.IsArray() || cfg == nil || !cfg.Enabled {
		return out
	}
	count := messages.Len()
	retained := cfg.RetainedMsgs
	if retained <= 0 {
		retained = FoldDefaultRetainedMsgs
	}
	keep := cfg.ReasoningExcerptBytes
	if keep <= 0 {
		keep = FoldDefaultExcerptBytes
	}
	if count <= 0 || retained >= count {
		return out // nothing ahead of the retained tail
	}
	limit := count - retained // messages [0, limit) are compression-eligible

	// Resolve the shared shrink policy ONCE, so this seam and the eager seam
	// shrink identically. `keep` is the threshold; the tail is capped to keep/2 so
	// a small excerpt budget keeps the tail proportional.
	cc := CompactConfig{Enabled: true, Threshold: keep, HeadBytes: cfg.CompactHeadBytes}
	tailDefault := cfg.CompactTailBytes
	if tailDefault <= 0 {
		tailDefault = CompactDefaultTailBytes
	}
	if tailCap := keep / 2; tailCap > 0 && tailCap < tailDefault {
		cc.TailBytes = tailCap
	} else {
		cc.TailBytes = tailDefault
	}

	// Deep-copy the whole transcript, then shrink only oversized bodies in place.
	// Every message slot, role/type, id and the ordering survive untouched.
	arr := messages.Clone()

	var set CoordSet
	compressedRaw := 0
	compressed := 0
	for i := 0; i < limit; i++ {
		if m := arr.At(i); m != nil {
			n, raw := compressMessageBodies(m, &cc, i, &set)
			compressed += n
			compressedRaw += raw
		}
	}
	if compressed == 0 {
		return out // no body exceeded the threshold -> caller uses the original
	}

	closet, evict := RenderCloset(&set, cfg.Closet, compressedRaw)
	out.ClosetEvict = evict
	if closet != "" {
		var body strings.Builder
		fmt.Fprintf(&body,
			"[compressed %d oversized tool-result body(ies) above; full bodies remain "+
				"in history — exact identifiers are conserved in the Coordinate Closet]\n\n",
			compressed)
		body.WriteString(closet)

		// Keep Anthropic role-alternation intact. Only the ends-with-user case
		// needs a bridge, and the note must stay LAST so the transcript never ends
		// on an assistant turn (which would read as a prefill).
		if tail := arr.At(arr.Len() - 1); tail != nil && tail.GetString("role") == "user" {
			ack := NewObject()
			ack.Set("role", NewString("assistant"))
			ack.Set("content", NewString(
				"Understood — identifiers from the compressed tool results are conserved below."))
			arr.Append(ack)
		}
		note := NewObject()
		note.Set("role", NewString("user"))
		note.Set("content", NewString(body.String()))
		arr.Append(note)
	}

	out.Messages = arr
	out.Folded = true // flag reused to mean "compressed"
	out.FoldedMsgs = compressed
	out.RetainedMsgs = retained
	return out
}

// FoldView produces a folded view: a synthetic user turn carrying the skeleton
// plus the Coordinate Closet, a synthetic assistant ack, then the retained tail
// verbatim.
//
// Never mutates its input and never touches the system prompt.
func FoldView(messages *JSONValue, cfg *FoldConfig, freeze *FoldFreeze) FoldResult {
	out := FoldResult{ClosetEvict: EvictNone}
	if messages == nil || !messages.IsArray() || cfg == nil || !cfg.Enabled {
		return out
	}

	count := messages.Len()
	retained := cfg.RetainedMsgs
	if retained <= 0 {
		retained = FoldDefaultRetainedMsgs
	}
	minFold := cfg.MinFoldMsgs
	if minFold <= 0 {
		minFold = FoldDefaultMinFoldMsgs
	}
	excerpt := cfg.ReasoningExcerptBytes
	if excerpt <= 0 {
		excerpt = FoldDefaultExcerptBytes
	}
	if count <= 0 || retained >= count || count-retained < minFold {
		return out // too short to fold cleanly
	}

	tailCap := FoldDefaultTailCapMsgs
	if freeze != nil && freeze.TailCapMsgs > 0 {
		tailCap = freeze.TailCapMsgs
	}
	if tailCap < retained {
		tailCap = retained // a cap below the retained band would re-epoch every turn
	}

	split := -1
	reused := false
	foldedBytes := 0
	var dig uint64

	// §3 fold-freeze: reuse the pinned boundary only when it is still a clean
	// boundary, the tail is within cap, AND the folded prefix is byte-for-byte
	// unchanged. The digest check turns a mid-run mutation into an epoch rather
	// than a false "reuse" claiming a warm cache it does not have.
	if freeze != nil && freeze.Active {
		fs := freeze.FrozenSplit
		if fs >= minFold && fs < count && (count-fs) <= tailCap && isCleanUserTurn(messages.At(fs)) {
			dig, foldedBytes = prefixDigest(messages, fs)
			if dig == freeze.PrefixDigest {
				split = fs
				reused = true
			}
		}
	}

	if !reused {
		// fresh boundary: first fold, freeze disabled, or an epoch advance
		desired := count - retained
		for s := desired; s >= minFold; s-- {
			if isCleanUserTurn(messages.At(s)) {
				split = s
				break
			}
		}
		if split < minFold {
			return out // no clean boundary leaves enough folded
		}
		dig, foldedBytes = prefixDigest(messages, split)
	}

	var set CoordSet
	var body strings.Builder
	fmt.Fprintf(&body,
		"[folded %d earlier message(s); skeleton below — exact identifiers are conserved in "+
			"the Coordinate Closet, full bodies remain in history]\n\n", split)
	for i := 0; i < split; i++ {
		skeletonMessage(&body, messages.At(i), i, excerpt, cfg.RegisterEnabled, &set)
	}

	closet, evict := RenderCloset(&set, cfg.Closet, foldedBytes)
	out.ClosetEvict = evict
	if closet != "" {
		body.WriteByte('\n')
		body.WriteString(closet)
	}

	arr := NewArray()
	fm := NewObject()
	fm.Set("role", NewString("user"))
	fm.Set("content", NewString(body.String()))
	arr.Append(fm)
	ack := NewObject()
	ack.Set("role", NewString("assistant"))
	ack.Set("content", NewString("Understood — continuing from the folded summary above."))
	arr.Append(ack)

	for i := split; i < count; i++ {
		if item := messages.At(i); item != nil {
			arr.Append(item.Clone())
		}
	}

	// Commit the freeze state only after a successful build. epochs++ counts
	// genuine boundary advances, not no-op re-commits of the same boundary.
	if freeze != nil {
		advanced := !reused && (!freeze.Active || freeze.FrozenSplit != split || freeze.PrefixDigest != dig)
		freeze.Active = true
		freeze.FrozenSplit = split
		freeze.PrefixDigest = dig
		if advanced {
			freeze.Epochs++
		}
	}

	out.Messages = arr
	out.Folded = true
	out.FoldedMsgs = split
	out.RetainedMsgs = count - split
	out.ReusedBoundary = reused
	return out
}
