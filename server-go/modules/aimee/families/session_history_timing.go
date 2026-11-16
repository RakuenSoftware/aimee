package families

import (
	"context"
	"fmt"
	"io"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

// Optional diagnostic metadata only. Neither this local ordinal nor any phase
// grants authority, identifies a session, or changes a transaction/deadline.
type historyTimingKey struct{}
type historyTimingPhase uint8

const (
	historyTimingLoad historyTimingPhase = iota
	historyTimingCheckpoint
	historyTimingEncode
	historyTimingAppend
)

var historyTimingOrdinal atomic.Uint64

func historyTimingPhaseName(phase historyTimingPhase) string {
	switch phase {
	case historyTimingLoad:
		return "history_load"
	case historyTimingCheckpoint:
		return "input_checkpoint"
	case historyTimingEncode:
		return "checkpoint_encode"
	case historyTimingAppend:
		return "checkpoint_append"
	default:
		return "other"
	}
}

func historyTimingLine(phase historyTimingPhase, ordinal uint64, elapsed time.Duration, bytes int) string {
	return fmt.Sprintf("AIMEE_HISTORY_TIMING call=%d phase=%s elapsed_us=%d input_bytes=%d\n",
		ordinal, historyTimingPhaseName(phase), elapsed.Microseconds(), bytes)
}

func historyTimingNoop() {}

func startHistoryTiming(ctx context.Context, phase historyTimingPhase, bytes int) (context.Context, func()) {
	return startHistoryTimingTo(ctx, phase, bytes, os.Stderr)
}

func startHistoryTimingTo(ctx context.Context, phase historyTimingPhase, bytes int, output io.Writer) (context.Context, func()) {
	if ctx == nil || os.Getenv("AIMEE_SESSION_HISTORY_TIMINGS") != "1" {
		return ctx, historyTimingNoop
	}
	ordinal, _ := ctx.Value(historyTimingKey{}).(uint64)
	if ordinal == 0 {
		ordinal = historyTimingOrdinal.Add(1)
		ctx = context.WithValue(ctx, historyTimingKey{}, ordinal)
	}
	started := time.Now()
	var finished sync.Once
	return ctx, func() {
		// Only fixed labels and numeric measurements enter stderr. The caller's
		// identifiers, JSON, SQL arguments, credentials and proofs never do.
		finished.Do(func() {
			_, _ = fmt.Fprint(output, historyTimingLine(phase, ordinal, time.Since(started), bytes))
		})
	}
}
