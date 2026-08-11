package economizer

import (
	"fmt"
	"sort"
	"strconv"
)

// Reducer state persistence.
//
// Ported from reduce_state_serialize / reduce_state_restore in
// src/modules/economizer/context_reduce.c.
//
// This is what makes eviction survive a process boundary: without it the page
// table is rebuilt empty every run, so a coordinate folded away in one run is
// simply gone in the next — the agent asks about it and nothing can tell it the
// content is pageable rather than lost.

// ReduceStateSerialMax bounds the serialized blob so it always fits the store
// row it is written to.
const ReduceStateSerialMax = 6144

// SerializeState renders st for persistence, or returns ok=false when it cannot
// produce something within the size bound.
//
// Keys are ranked most-recently-surfaced first so the size cap drops the COLDEST
// first. A key the agent has never reached for is the weakest candidate to carry
// into the next run, so never-surfaced keys (last turn -1) sink to the end.
func SerializeState(st *ReduceState) (string, bool) {
	if st == nil {
		return "", false
	}

	root := NewObject()
	root.Set("turn", NewNumber(float64(st.Turn)))

	fz := NewObject()
	fz.Set("active", NewNumber(boolToNum(st.Freeze.Active)))
	fz.Set("frozen_split", NewNumber(float64(st.Freeze.FrozenSplit)))
	fz.Set("tail_cap_msgs", NewNumber(float64(st.Freeze.TailCapMsgs)))
	fz.Set("epochs", NewNumber(float64(st.Freeze.Epochs)))
	// The digest is 64-bit; a JSON number is a double and would lose the low bits,
	// so it travels as a hex string. Losing digest fidelity would silently defeat
	// the fold's staleness check — the one guard that stops a restored boundary
	// serving an obsolete prefix.
	fz.Set("prefix_digest", NewString(strconv.FormatUint(st.Freeze.PrefixDigest, 16)))
	root.Set("freeze", fz)

	keys := NewArray()
	root.Set("recall", keys)

	// Reserve the trailing counters UP FRONT so the budget check below measures
	// the real final size.
	//
	// DELIBERATE FIX vs the C original, which adds these only after the loop and
	// so under-counts by ~40 bytes. Verified against the real C: with 400 long
	// path keys, reduce_state_serialize returns NULL — meaning the WHOLE state
	// (freeze boundary and page table) is silently not persisted, so cross-run
	// recall stops working and the freeze restarts cold every run, with nothing
	// reporting a failure. Short keys hide it, which is why the C suite passes.
	root.Set("recall_kept", NewNumber(0))
	root.Set("recall_dropped", NewNumber(0))

	type ranked struct {
		key      string
		lastTurn int
		idx      int
	}
	var order []ranked
	if st.Recall != nil {
		for i, k := range st.Recall.Keys() {
			order = append(order, ranked{key: k, lastTurn: st.Recall.LastTurn(k), idx: i})
		}
	}
	sort.SliceStable(order, func(a, b int) bool {
		if order[a].lastTurn != order[b].lastTurn {
			return order[a].lastTurn > order[b].lastTurn // higher first; -1 sinks
		}
		return order[a].idx < order[b].idx
	})

	kept, dropped := 0, 0
	for i, r := range order {
		if r.key == "" {
			continue
		}
		// Budget against the serialized size SO FAR, not a guessed row width. The
		// allowance covers this entry's JSON overhead plus digit growth in the two
		// counters, which are already present in `root` and therefore counted.
		used := len(PrintJSONUnformatted(root))
		if used+len(r.key)+48 > ReduceStateSerialMax {
			dropped = len(order) - i
			break
		}
		e := NewObject()
		e.Set("k", NewString(r.key))
		e.Set("t", NewNumber(float64(r.lastTurn)))
		keys.Append(e)
		kept++
	}
	root.Set("recall_kept", NewNumber(float64(kept)))
	root.Set("recall_dropped", NewNumber(float64(dropped)))

	out := PrintJSONUnformatted(root)
	if len(out) > ReduceStateSerialMax {
		// Belt and braces: never hand back something the store would truncate.
		return "", false
	}
	return out, true
}

func boolToNum(b bool) float64 {
	if b {
		return 1
	}
	return 0
}

// RestoreState loads a serialized blob into st.
//
// ALL-OR-NOTHING: it builds into a local and commits in one go, because a
// half-applied freeze (a split without its digest) is worse than no state at
// all — it would serve a boundary whose staleness can no longer be checked.
//
// `Reduced` is deliberately NOT restored: it is per-REQUEST provenance, and
// carrying it across would make the next request believe it had already been
// reduced and skip the work entirely.
func RestoreState(st *ReduceState, blob string) error {
	if st == nil || blob == "" {
		return fmt.Errorf("economizer: nothing to restore")
	}
	root := ParseJSON(blob)
	if root == nil || root.Kind != JSONObject {
		return fmt.Errorf("economizer: state is not valid JSON")
	}

	var tmp ReduceState
	tmp.Recall = NewRecallIndex()

	if t := root.Get("turn"); t != nil && t.Kind == JSONNumber {
		tmp.Turn = int(t.Num)
	}

	if fz := root.Get("freeze"); fz != nil && fz.Kind == JSONObject {
		if v := fz.Get("active"); v != nil && v.Kind == JSONNumber {
			tmp.Freeze.Active = v.Num != 0
		}
		if v := fz.Get("frozen_split"); v != nil && v.Kind == JSONNumber {
			tmp.Freeze.FrozenSplit = int(v.Num)
		}
		if v := fz.Get("tail_cap_msgs"); v != nil && v.Kind == JSONNumber {
			tmp.Freeze.TailCapMsgs = int(v.Num)
		}
		if v := fz.Get("epochs"); v != nil && v.Kind == JSONNumber {
			tmp.Freeze.Epochs = int(v.Num)
		}
		if v := fz.Get("prefix_digest"); v != nil && v.Kind == JSONString {
			// A malformed digest parses to 0, which cannot match any real prefix, so
			// the next fold re-epochs rather than trusting a boundary it cannot verify.
			d, _ := strconv.ParseUint(v.Str, 16, 64)
			tmp.Freeze.PrefixDigest = d
		}
	}

	if keys := root.Get("recall"); keys.IsArray() {
		for _, e := range keys.Items {
			k := e.Get("k")
			if k == nil || k.Kind != JSONString || k.Str == "" {
				continue
			}
			tmp.Recall.Add(k.Str)
			if lt := e.Get("t"); lt != nil && lt.Kind == JSONNumber {
				tmp.Recall.SetLastTurn(k.Str, int(lt.Num))
			}
		}
	}

	*st = tmp
	st.Reduced = false
	return nil
}
