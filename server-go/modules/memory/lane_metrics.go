package memory

import "sync"

// Recall attribution measures the final store result, before any caller's
// context packing. It never changes ranking and contains no record identities.
type recallLanes map[int64]uint16

const (
	laneLexical uint16 = 1 << iota
	laneAlias
	laneEntity
	laneSummary
	laneEvent
	laneChunk
	laneUnit
	laneTemporal
	laneSemantic
	laneLike
	laneCode
	laneGraph
)

var laneNames = [...]string{"lexical", "alias", "entity", "summary", "event", "chunk", "unit", "temporal", "semantic", "like", "code", "graph"}

func (lanes recallLanes) add(records []Record, source uint16) {
	if lanes == nil {
		return
	}
	for _, record := range records {
		lanes[record.ID] |= source
	}
}

func laneOutcomes(route string, matches []Record, served int, sources recallLanes) map[string]int64 {
	out := map[string]int64{}
	ids := map[int64]bool{}
	for _, record := range matches[:max(0, min(served, len(matches)))] {
		ids[record.ID] = true
	}
	for bit, name := range laneNames {
		var candidates, placed int64
		for id, mask := range sources {
			if mask&(1<<bit) != 0 {
				candidates++
				if ids[id] {
					placed++
				}
			}
		}
		if candidates == 0 {
			continue
		}
		prefix := "memory.query.lane." + name
		out[prefix+".candidates"] = candidates
		out[prefix+".served"] = placed
		if placed == 0 {
			out[prefix+".shutout"] = 1
		}
		if route != "" {
			out["memory.query.route."+route+".lane."+name+".served"] = placed
		}
	}
	return out
}

var laneMetricState = struct {
	sync.Mutex
	values map[string]int64
}{values: map[string]int64{}}

func (lanes recallLanes) observe(matches []Record) {
	route := "lexical"
	for _, mask := range lanes {
		if mask&(laneSemantic|laneGraph) != 0 {
			route = "hybrid"
			break
		}
	}
	values := laneOutcomes(route, matches, len(matches), lanes)
	laneMetricState.Lock()
	defer laneMetricState.Unlock()
	for key, value := range values {
		laneMetricState.values[key] += value
	}
}

func laneMetrics() map[string]int64 {
	laneMetricState.Lock()
	defer laneMetricState.Unlock()
	out := make(map[string]int64, len(laneMetricState.values))
	for key, value := range laneMetricState.values {
		out[key] = value
	}
	return out
}
