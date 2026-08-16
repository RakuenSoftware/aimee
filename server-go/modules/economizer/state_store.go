package economizer

import "context"

// StateStore is where a seam's per-conversation reducer state lives between
// turns. The economizer owns this state, so it loads and saves it itself rather
// than having a caller carry the blob in and back out: a caller that ferries
// state has to know the module has state at all, which is the coupling the
// module boundary exists to remove.
//
// server-go/db1.Client satisfies this over the bus. It is an interface here so
// the module depends on the capability rather than on DB1.
type StateStore interface {
	// LoadState returns the stored blob and whether one was found. A miss is
	// not an error: the first turn of a conversation has no state.
	LoadState(ctx context.Context, key string) (string, bool, error)
	SaveState(ctx context.Context, key, blob string) error
}

// restoreState seats the reducer state for this turn.
//
// Every failure here is deliberately soft, and they all land in the same place:
// a cold start. Reducer state is an optimization -- it carries a freeze
// boundary and a page table that make the NEXT turn cheaper -- so losing it
// costs one cold turn, while failing the request costs the caller its whole
// reduction. No store configured, no key, an unreachable store, and an
// unreadable blob are therefore all the same outcome by design.
func restoreState(store StateStore, stats *GatewayStatsStore, key string) *ReduceState {
	st := &ReduceState{Recall: NewRecallIndex()}
	if store == nil || key == "" {
		return st
	}
	blob, found, err := store.LoadState(context.Background(), key)
	if err != nil {
		statsIncStore(stats, StatStateUnavailable)
		return st
	}
	if !found || blob == "" {
		return st
	}
	if err := RestoreState(st, blob); err != nil {
		// Discard rather than fail: see above. Counted as unavailable because
		// the effect is identical -- this turn runs cold.
		statsIncStore(stats, StatStateUnavailable)
		return &ReduceState{Recall: NewRecallIndex()}
	}
	// The stored turn is the one that WROTE this state, so this turn is the
	// next one. The caller used to compute this; it is the module's to know.
	st.Turn++
	if st.Recall == nil {
		st.Recall = NewRecallIndex()
	}
	return st
}

// persistState writes the state this turn produced. A blob that will not
// serialize is left unwritten, which the next turn reads as "keep what you
// have" rather than as a cleared boundary.
func persistState(store StateStore, stats *GatewayStatsStore, key string, st *ReduceState) {
	if store == nil || key == "" {
		return
	}
	blob, ok := SerializeState(st)
	if !ok || blob == "" {
		return
	}
	// A failed write costs the next turn its warmth and nothing else, so it is
	// not worth failing a reduction that already succeeded -- but it is worth
	// counting, because nothing else would show it.
	if err := store.SaveState(context.Background(), key, blob); err != nil {
		statsIncStore(stats, StatStateSaveFailed)
	}
}

// statsIncStore counts a store outcome when there is somewhere to count it.
func statsIncStore(stats *GatewayStatsStore, counter string) {
	if stats == nil {
		return
	}
	stats.Inc(counter)
}
