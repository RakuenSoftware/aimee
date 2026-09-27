package executionpolicy

import (
	"fmt"
	"testing"
)

// Benchmark the admission reducer with growing retained task history. This
// excludes database locking/commit, source-owner transport and tool execution;
// live API timings are reported separately rather than conflated with this cost.
func BenchmarkGovernedActionAdmission(b *testing.B) {
	for _, history := range []int{0, 32, 256} {
		b.Run(fmt.Sprintf("retained_%d", history), func(b *testing.B) {
			i, f, now := actionFixture()
			var state []byte
			for n := 0; n <= history; n++ {
				i.ID = fmt.Sprintf("action-%d", n)
				i.IdempotencyKey = i.ID
				f.IntentDigest = actionDigest(i)
				var d ActionDecision
				var err error
				state, d, err = GovernedAction("alice", "root", state, "prepare", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
				if err != nil || !d.Allowed {
					b.Fatal(err, d)
				}
				if n < history {
					state, d, err = GovernedAction("alice", "root", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
					if err != nil || !d.Allowed {
						b.Fatal(err, d)
					}
				}
			}
			b.ReportAllocs()
			b.ResetTimer()
			for n := 0; n < b.N; n++ {
				_, d, err := GovernedAction("alice", "root", state, "admit", i, i.Class, f, nil, ActionCompositionPolicy{}, now)
				if err != nil || !d.Allowed {
					b.Fatal(err, d)
				}
			}
		})
	}
}
