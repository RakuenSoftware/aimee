package families

import (
	"bytes"
	"context"
	"strings"
	"testing"
	"time"
)

func TestHistoryTimingDisabledPreservesContextAndEmitsNothing(t *testing.T) {
	t.Setenv("AIMEE_SESSION_HISTORY_TIMINGS", "0")
	ctx := context.Background()
	var output bytes.Buffer
	before := historyTimingOrdinal.Load()
	actual, finish := startHistoryTimingTo(ctx, historyTimingLoad, -1, &output)
	finish()
	if actual != ctx || output.Len() != 0 || historyTimingOrdinal.Load() != before {
		t.Fatal("disabled diagnostic changed context, ordinal or output")
	}
}

func TestHistoryTimingNestedScopeKeepsOnlyLocalNumericOrdinal(t *testing.T) {
	t.Setenv("AIMEE_SESSION_HISTORY_TIMINGS", "1")
	type unrelatedKey struct{}
	original := context.WithValue(context.Background(), unrelatedKey{}, "must-not-appear")
	var output bytes.Buffer
	outer, finish := startHistoryTimingTo(original, historyTimingCheckpoint, -1, &output)
	inner, finishInner := startHistoryTimingTo(outer, historyTimingLoad, -1, &output)
	id, ok := outer.Value(historyTimingKey{}).(uint64)
	if !ok || id == 0 || inner.Value(historyTimingKey{}) != id || original.Value(historyTimingKey{}) != nil {
		t.Fatal("diagnostic scope did not retain one fresh local ordinal")
	}
	finishInner()
	finish()
	if strings.Contains(output.String(), "must-not-appear") || strings.Count(output.String(), "AIMEE_HISTORY_TIMING ") != 2 {
		t.Fatal("unexpected diagnostic fields or line count")
	}
}

func TestHistoryTimingFinishRunsOnceIncludingDeferredFinish(t *testing.T) {
	t.Setenv("AIMEE_SESSION_HISTORY_TIMINGS", "1")
	var output bytes.Buffer
	_, finish := startHistoryTimingTo(context.Background(), historyTimingEncode, -1, &output)
	func() {
		defer finish()
		finish()
	}()
	if strings.Count(output.String(), "AIMEE_HISTORY_TIMING ") != 1 {
		t.Fatal("explicit and deferred completion duplicated a timing")
	}
}

func TestHistoryTimingFieldsAreFixedAndUnknownBytesStayUnknown(t *testing.T) {
	line := historyTimingLine(historyTimingPhase(255), 7, 23*time.Microsecond, -1)
	if line != "AIMEE_HISTORY_TIMING call=7 phase=other elapsed_us=23 input_bytes=-1\n" {
		t.Fatal("unknown phase or numeric fields changed")
	}
	for _, phase := range []historyTimingPhase{historyTimingLoad, historyTimingCheckpoint, historyTimingEncode, historyTimingAppend} {
		if strings.ContainsAny(historyTimingPhaseName(phase), "\n\r\t ") {
			t.Fatal("phase label is not a closed token")
		}
	}
}
