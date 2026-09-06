package memory

import (
	"sync/atomic"
	"time"
)

type RuntimeMetrics struct {
	Created    int64   `json:"created"`
	Resolved   int64   `json:"resolved"`
	Expired    int64   `json:"expired"`
	Surfaced   int64   `json:"surfaced"`
	Triggered  int64   `json:"triggered"`
	Completed  int64   `json:"completed"`
	Calls      int64   `json:"calls"`
	AverageMS  float64 `json:"average_ms"`
	MaximumMS  float64 `json:"maximum_ms"`
	Assemblies int64   `json:"assemblies"`
	Starts     int64   `json:"starts"`
}

type durationCounters struct {
	calls atomic.Int64
	total atomic.Int64
	max   atomic.Int64
}

func (m *durationCounters) observe(start time.Time) {
	nanos := time.Since(start).Nanoseconds()
	m.calls.Add(1)
	m.total.Add(nanos)
	for old := m.max.Load(); nanos > old && !m.max.CompareAndSwap(old, nanos); old = m.max.Load() {
	}
}

func (m *durationCounters) snapshot() (int64, float64, float64) {
	calls := m.calls.Load()
	if calls == 0 {
		return 0, 0, 0
	}
	return calls, float64(m.total.Load()) / float64(calls) / float64(time.Millisecond),
		float64(m.max.Load()) / float64(time.Millisecond)
}

var runtimeMetricState struct {
	directiveCreated   atomic.Int64
	directiveResolved  atomic.Int64
	directiveExpired   atomic.Int64
	directiveSurfaced  atomic.Int64
	directiveCalls     durationCounters
	prospectiveTrigger atomic.Int64
	prospectiveDone    atomic.Int64
	prospectiveExpired atomic.Int64
	prospectiveCalls   durationCounters
	recallAssemblies   atomic.Int64
	recallStarts       atomic.Int64
	recallCalls        durationCounters
}

func directiveMetrics() RuntimeMetrics {
	calls, average, maximum := runtimeMetricState.directiveCalls.snapshot()
	return RuntimeMetrics{Created: runtimeMetricState.directiveCreated.Load(),
		Resolved: runtimeMetricState.directiveResolved.Load(), Expired: runtimeMetricState.directiveExpired.Load(),
		Surfaced: runtimeMetricState.directiveSurfaced.Load(), Calls: calls, AverageMS: average, MaximumMS: maximum}
}

func prospectiveMetrics() RuntimeMetrics {
	calls, average, maximum := runtimeMetricState.prospectiveCalls.snapshot()
	return RuntimeMetrics{Triggered: runtimeMetricState.prospectiveTrigger.Load(),
		Completed: runtimeMetricState.prospectiveDone.Load(), Expired: runtimeMetricState.prospectiveExpired.Load(),
		Calls: calls, AverageMS: average, MaximumMS: maximum}
}

func recallMetrics() RuntimeMetrics {
	_, average, maximum := runtimeMetricState.recallCalls.snapshot()
	return RuntimeMetrics{Assemblies: runtimeMetricState.recallAssemblies.Load(),
		Starts: runtimeMetricState.recallStarts.Load(), AverageMS: average, MaximumMS: maximum}
}
