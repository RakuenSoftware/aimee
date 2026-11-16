package families

import (
	"context"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
)

// Only complete committed API snapshots identify calls; deltas fix their order.
// Native snapshots require native custody recovery and cannot use this path.
func operatorInterruptionSources(ctx context.Context, q store.Queryer, session, turn, actor string) ([]canonical.ToolInterruptionSource, error) {
	observed := []interruptionObservation{}
	last := "0"
	bytes := 0
	for {
		rows, err := q.Query(ctx, `SELECT sequence::text,event_id,actor,payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND sequence>$3::bigint AND (payload::jsonb->>'kind'='message' OR (payload::jsonb->>'kind'='delta' AND payload::jsonb->'payload'->>'type'='TURN_START')) ORDER BY session_ir_events.sequence LIMIT 8`, session, turn, last)
		if err != nil {
			return nil, err
		}
		page := 0
		err = func() error {
			defer rows.Close()
			for rows.Next() {
				source := canonical.ToolInterruptionSource{Session: session, Turn: turn}
				var raw string
				if err := rows.Scan(&source.Sequence, &source.EventID, &source.Actor, &raw); err != nil {
					return err
				}
				last = source.Sequence
				page++
				bytes += len(raw)
				if bytes > 16<<20 || len(observed) >= 4096 {
					return fmt.Errorf("operator interruption source capacity exceeded")
				}
				var event nativeOwnedMessage
				if source.Actor != actor || !canonical.ValidSessionEvent([]byte(raw)) || json.Unmarshal([]byte(raw), &event) != nil || event.Owner != nil {
					return fmt.Errorf("operator interruption requires exact owned API source")
				}
				observed = append(observed, interruptionObservation{Source: source, Event: json.RawMessage(raw), Order: uint64(len(observed) + 1)})
			}
			return rows.Err()
		}()
		if err != nil {
			return nil, err
		}
		if page < 8 {
			break
		}
	}
	return nativeUnansweredCalls(observed)
}
func operatorRecordInterruptions(ctx context.Context, q store.Queryer, session, turn, actor, terminal string, sources []canonical.ToolInterruptionSource) error {
	for _, source := range sources {
		receipt := canonical.ToolInterruption{Version: 1, Source: source, Owner: canonical.ToolInterruptionOwner{OperatorCancel: operatorCancelPrefix + turn}, Terminal: terminal, Outcome: "unknown", Retry: "explicit_permission_required"}
		raw, err := json.Marshal(struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ToolInterruption `json:"payload"`
		}{"tool_interruption", receipt})
		if err != nil || !canonical.ValidSessionEvent(raw) {
			return fmt.Errorf("invalid operator interruption receipt")
		}
		id := fmt.Sprintf("operator-interruption:%s:%s:%d", turn, source.Sequence, source.BlockIndex)
		fields := []string{session, id, turn, actor, string(raw)}
		var sequence int64
		// Only this typed owner append follows its terminal in the same transaction.
		if err = q.QueryRow(ctx, sessionIRInsertSQL, session, id, turn, actor, string(raw)).Scan(&sequence); err != nil {
			return err
		}
		if err = enqueueLearningProjection(ctx, q, fields, sequence); err != nil {
			return err
		}
	}
	return nil
}
func operatorReceiptAuthority(ctx context.Context, q store.Queryer, r canonical.ToolInterruption) error {
	s := r.Source
	if r.Owner.OperatorCancel != operatorCancelPrefix+s.Turn {
		return fmt.Errorf("operator interruption terminal binding mismatch")
	}
	observed, err := strconv.ParseUint(s.Sequence, 10, 63)
	if err != nil {
		return err
	}
	terminal, err := strconv.ParseUint(r.Terminal, 10, 63)
	if err != nil || observed >= terminal {
		return fmt.Errorf("operator interruption source is not before cancellation")
	}
	var payload string
	err = q.QueryRow(ctx, `SELECT e.payload FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id AND s.principal=e.actor WHERE e.session_id=$1 AND e.sequence=$2::bigint AND e.event_id=$3 AND e.turn_id=$4 AND e.actor=$5`, s.Session, r.Terminal, r.Owner.OperatorCancel, s.Turn, s.Actor).Scan(&payload)
	if err != nil {
		return err
	}
	if payload != operatorCancelPayload {
		return fmt.Errorf("operator interruption lacks privileged cancellation")
	}
	return nil
}
