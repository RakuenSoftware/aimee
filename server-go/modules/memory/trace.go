package memory

import "sync"

type RecallRejection struct {
	MemoryID int64  `json:"memory_id"`
	Lane     string `json:"lane"`
	Gate     string `json:"gate"`
}

var recallTraceState struct {
	sync.Mutex
	active     bool
	rejections []RecallRejection
}

func recallTraceBegin() {
	recallTraceState.Lock()
	recallTraceState.active = true
	recallTraceState.rejections = nil
	recallTraceState.Unlock()
}

func recallTraceEnd() {
	recallTraceState.Lock()
	recallTraceState.active = false
	recallTraceState.Unlock()
}

func recallTraceSnapshot() []RecallRejection {
	recallTraceState.Lock()
	defer recallTraceState.Unlock()
	return append([]RecallRejection(nil), recallTraceState.rejections...)
}
