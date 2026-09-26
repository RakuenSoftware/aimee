package memory

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"sort"
	"time"
)

// Health metadata is private serving evidence. These types are not accepted
// from recall callers. The owner authenticates and filters the population before
// aggregation; reports intentionally contain no record, family or task IDs.
type healthRecord struct {
	Positions  []healthPosition `json:"final_positions,omitempty"`
	RecordID   string           `json:"record_id"`
	VersionID  string           `json:"version_id"`
	Kind       string           `json:"kind"`
	Family     string           `json:"family,omitempty"`
	LowTrust   *bool            `json:"low_trust"`
	Historical bool             `json:"historical"`
	// Nil means no release-time verifier label, not a verified safe delivery.
	LifecycleViolation *bool `json:"lifecycle_violation"`
}

type healthInvocation struct {
	Request       string                 `json:"request_id,omitempty"`
	Labels        *healthSelectionLabels `json:"selection_labels,omitempty"`
	MetadataGaps  []string               `json:"metadata_gaps,omitempty"`
	Attempt       string                 `json:"attempt"`
	Binding       string                 `json:"binding"`
	At            time.Time              `json:"at"`
	Namespace     string                 `json:"namespace"`
	Principal     string                 `json:"principal"`
	Project       string                 `json:"project"`
	Workspace     string                 `json:"workspace"`
	Purpose       string                 `json:"purpose"`
	QueryClass    string                 `json:"query_class"`
	Fingerprint   string                 `json:"query_fingerprint"`
	Task          string                 `json:"task"`
	Turn          string                 `json:"turn"`
	PreviousTurn  string                 `json:"previous_turn"`
	Stage         string                 `json:"stage"`
	SamplePPM     int                    `json:"sampling_probability_ppm"`
	SamplingEpoch string                 `json:"sampling_epoch"`
	Records       []healthRecord         `json:"records"`
}

type healthPopulation struct {
	Namespace  string    `json:"namespace"`
	Principal  string    `json:"-"`
	Project    string    `json:"project"`
	Workspace  string    `json:"workspace"`
	Purpose    string    `json:"purpose"`
	QueryClass string    `json:"query_class"`
	Stage      string    `json:"stage"`
	From       time.Time `json:"from"`
	Until      time.Time `json:"until"`
}

func (p healthPopulation) includes(e healthInvocation) bool {
	return p.Namespace != "" && p.Principal != "" && e.Namespace == p.Namespace && e.Principal == p.Principal &&
		e.Project == p.Project && e.Workspace == p.Workspace && e.Purpose == p.Purpose && e.QueryClass == p.QueryClass &&
		e.Stage == p.Stage && !e.At.Before(p.From) && e.At.Before(p.Until)
}

// Length-delimited JSON and a domain separator prevent concatenation and
// cross-namespace collisions. The secret key is never a public receipt digest.
func healthQueryFingerprint(key []byte, namespace, query string) string {
	if len(key) < 32 || namespace == "" {
		return ""
	}
	m := hmac.New(sha256.New, key)
	raw, _ := json.Marshal([]string{"memory-health-query-v1", namespace, query})
	_, _ = m.Write(raw)
	return hex.EncodeToString(m.Sum(nil))
}

type healthConcentration struct {
	Occurrences int      `json:"occurrences"`
	Distinct    int      `json:"distinct"`
	Top1Share   *float64 `json:"top_1_share"`
	Top5Share   *float64 `json:"top_5_share"`
	HHI         *float64 `json:"hhi"`
	Simpson     *float64 `json:"simpson_diversity"`
	Entropy     *float64 `json:"entropy_bits"`
	Normalized  *float64 `json:"normalized_entropy"`
}

func healthConcentrationOf(counts map[string]int) healthConcentration {
	r := healthConcentration{Distinct: len(counts)}
	ordered := make([]int, 0, len(counts))
	for _, c := range counts {
		r.Occurrences += c
		ordered = append(ordered, c)
	}
	if r.Occurrences == 0 {
		return r
	}
	sort.Sort(sort.Reverse(sort.IntSlice(ordered)))
	hhi, entropy, top5 := 0.0, 0.0, 0.0
	for i, c := range ordered {
		p := float64(c) / float64(r.Occurrences)
		hhi += p * p
		entropy -= p * math.Log2(p)
		if i < 5 {
			top5 += p
		}
	}
	top1, simpson := float64(ordered[0])/float64(r.Occurrences), 1-hhi
	r.Top1Share, r.Top5Share, r.HHI, r.Simpson, r.Entropy = &top1, &top5, &hhi, &simpson, &entropy
	if r.Distinct > 1 {
		normalized := entropy / math.Log2(float64(r.Distinct))
		r.Normalized = &normalized
	}
	return r
}

type healthRatio struct {
	Numerator   int      `json:"numerator"`
	Denominator int      `json:"denominator"`
	Unknown     int      `json:"unknown"`
	Rate        *float64 `json:"rate"`
}

func (r *healthRatio) finish() {
	if r.Denominator > 0 {
		v := float64(r.Numerator) / float64(r.Denominator)
		r.Rate = &v
	}
}

type healthRuns struct {
	Lengths      map[int]int `json:"retained_contiguous_run_lengths"`
	UnknownStart int         `json:"runs_with_unknown_start"`
	UnknownEnd   int         `json:"runs_with_unknown_end"`
}

type healthMetrics struct {
	ByKind                  map[string]healthConcentration `json:"record_concentration_by_kind"`
	Labels                  healthLabelMetrics             `json:"labelled_selection"`
	LowTrustFamilyFanout    map[int]int                    `json:"low_trust_tasks_per_family"`
	UnknownFanout           int                            `json:"low_trust_occurrences_without_task"`
	Version                 int                            `json:"aggregate_version"`
	Population              healthPopulation               `json:"population"`
	Invocations             int                            `json:"retained_invocations"`
	Sampled                 bool                           `json:"sampled"`
	ConcentrationPopulation string                         `json:"concentration_population"`
	Records                 healthConcentration            `json:"records"`
	Versions                healthConcentration            `json:"versions"`
	Runs                    healthRuns                     `json:"consecutive_appearances"`
	Repeat                  healthRatio                    `json:"repeat_serving"`
	Lifecycle               healthRatio                    `json:"current_lifecycle_reentry"`
	FamilyCounts            map[int]int                    `json:"families_per_invocation"`
	UnknownOrigins          int                            `json:"unknown_origin_occurrences"`
	LowTrustFanout          map[int]int                    `json:"low_trust_tasks_per_record"`
	UnknownTrust            int                            `json:"unknown_trust_occurrences"`
	// Horvitz-Thompson total and design-based standard error for independent
	// Bernoulli sampling of entire invocations. Never used as exact integrity
	// counters or as an uncertainty claim for the concentration ratios above.
	EstimatedOccurrences *float64 `json:"estimated_record_occurrences"`
	EstimateSE           *float64 `json:"estimated_record_occurrences_standard_error"`
	EstimateMethod       string   `json:"estimate_method"`
	Unsupported          []string `json:"unmeasured_metrics"`
}

// Input must already be reconciled by attempt and have valid sampling metadata.
// Explicit previous-turn links prevent retention gaps, interleaved tasks and
// invocation sampling from creating fictitious adjacent turns. Retries of one
// turn contribute exposure per dispatch but are unioned for repeat membership.
func aggregateHealth(events []healthInvocation, population healthPopulation) (healthMetrics, error) {
	r := healthMetrics{Version: 1, Population: population, ConcentrationPopulation: "retained_invocations",
		FamilyCounts: map[int]int{}, LowTrustFanout: map[int]int{}, LowTrustFamilyFanout: map[int]int{}, EstimateMethod: "invocation_bernoulli_horvitz_thompson",
		Unsupported: []string{"sole_support_displacement", "arm_contamination", "fusion_recovery"}}
	if population.Namespace == "" || population.Principal == "" || population.Purpose == "" || population.QueryClass == "" || !population.Until.After(population.From) {
		return r, errors.New("incomplete health population")
	}
	switch population.Stage {
	case "dispatched", "assembled_unsent", "unknown_dispatch", "network_uncertain":
	default:
		return r, errors.New("unsupported serving stage")
	}
	filtered := make([]healthInvocation, 0, len(events))
	seenAttempts := map[string]string{}
	for _, e := range events {
		if !population.includes(e) {
			continue
		}
		if e.Attempt == "" || e.Binding == "" || e.SamplePPM <= 0 || e.SamplePPM > 1000000 || e.SamplingEpoch == "" || len(e.Records) > 256 {
			return r, errors.New("invalid health invocation")
		}
		for _, record := range e.Records {
			if record.RecordID == "" || record.VersionID == "" {
				return r, errors.New("missing health record identity")
			}
		}
		digest := releaseDigest(e)
		if previous, exists := seenAttempts[e.Attempt]; exists {
			if previous != digest {
				return r, errors.New("unreconciled health invocation")
			}
			continue
		}
		seenAttempts[e.Attempt] = digest
		filtered = append(filtered, e)
	}
	events = filtered
	records, versions := map[string]int{}, map[string]int{}
	kindRecords := map[string]map[string]int{}
	taskRecords := map[string]map[string]bool{}
	taskFamilies := map[string]map[string]bool{}
	turns := map[string]map[string]bool{}
	turnTimes := map[string]time.Time{}
	turnPrevious := map[string]string{}
	incompleteTurns := map[string]bool{}
	turnPredecessor := map[string]string{}
	children := map[string]int{}
	turnKey := func(task, turn string) string { raw, _ := json.Marshal([]string{task, turn}); return string(raw) }
	for _, e := range events {
		if !population.includes(e) || e.Task == "" || e.Turn == "" {
			continue
		}
		k := turnKey(e.Task, e.Turn)
		if turns[k] == nil {
			turns[k] = map[string]bool{}
			turnTimes[k] = e.At
			turnPrevious[k] = e.PreviousTurn
			if e.PreviousTurn != "" {
				turnPredecessor[k] = turnKey(e.Task, e.PreviousTurn)
				children[turnPredecessor[k]]++
			}
		} else if turnPrevious[k] != e.PreviousTurn {
			incompleteTurns[k] = true
		}
		if e.SamplePPM != 1000000 {
			incompleteTurns[k] = true
		}
		if e.At.Before(turnTimes[k]) {
			turnTimes[k] = e.At
		}
		for _, record := range e.Records {
			turns[k][record.RecordID] = true
		}
	}
	for k, count := range children {
		if count > 1 {
			incompleteTurns[k] = true
		}
	}
	r.Runs = healthRunDistribution(turns, turnPredecessor, turnTimes, incompleteTurns)
	variance, total := 0.0, 0.0
	for _, e := range events {
		if !population.includes(e) {
			continue
		}
		if err := r.Labels.add(e.Labels); err != nil {
			return r, err
		}
		r.Invocations++
		r.Sampled = r.Sampled || e.SamplePPM != 1000000
		seen, seenVersions, families := map[string]bool{}, map[string]bool{}, map[string]bool{}
		for _, record := range e.Records {
			versionKey, _ := json.Marshal([]string{record.RecordID, record.VersionID})
			if !seenVersions[string(versionKey)] {
				versions[string(versionKey)]++
				seenVersions[string(versionKey)] = true
			}
			if seen[record.RecordID] {
				continue
			}
			seen[record.RecordID] = true
			records[record.RecordID]++
			kind := record.Kind
			if kind == "" {
				kind = "unknown"
			}
			if kindRecords[kind] == nil {
				kindRecords[kind] = map[string]int{}
			}
			kindRecords[kind][record.RecordID]++
			if record.Family == "" {
				r.UnknownOrigins++
			} else {
				families[record.Family] = true
			}
			if record.LowTrust == nil {
				r.UnknownTrust++
			} else if *record.LowTrust && e.Task == "" {
				r.UnknownFanout++
			} else if *record.LowTrust && e.Task != "" {
				if taskRecords[record.RecordID] == nil {
					taskRecords[record.RecordID] = map[string]bool{}
				}
				taskRecords[record.RecordID][e.Task] = true
				if record.Family != "" {
					if taskFamilies[record.Family] == nil {
						taskFamilies[record.Family] = map[string]bool{}
					}
					taskFamilies[record.Family][e.Task] = true
				}
			}
			if !record.Historical {
				if record.LifecycleViolation == nil {
					r.Lifecycle.Unknown++
				} else {
					r.Lifecycle.Denominator++
					if *record.LifecycleViolation {
						r.Lifecycle.Numerator++
					}
				}
			}
			previousKey, currentKey := turnKey(e.Task, e.PreviousTurn), turnKey(e.Task, e.Turn)
			previous := turns[previousKey]
			if e.SamplePPM != 1000000 || e.Task == "" || e.Turn == "" || e.PreviousTurn == "" || e.PreviousTurn == e.Turn || previous == nil || incompleteTurns[previousKey] || incompleteTurns[currentKey] || !turnTimes[previousKey].Before(turnTimes[currentKey]) {
				r.Repeat.Unknown++
			} else {
				r.Repeat.Denominator++
				if previous[record.RecordID] {
					r.Repeat.Numerator++
				}
			}
		}
		r.FamilyCounts[len(families)]++
		p := float64(e.SamplePPM) / 1000000
		y := float64(len(seen))
		total += y / p
		variance += (1 - p) * y * y / (p * p)
	}
	for _, tasks := range taskRecords {
		r.LowTrustFanout[len(tasks)]++
	}
	for _, tasks := range taskFamilies {
		r.LowTrustFamilyFanout[len(tasks)]++
	}
	r.Labels.finish()
	r.Unsupported = nil
	if r.Labels.Displacement.Denominator == 0 {
		r.Unsupported = append(r.Unsupported, "sole_support_displacement")
	}
	contamination, recovery := 0, 0
	for _, ratio := range r.Labels.Contamination {
		contamination += ratio.Denominator
	}
	for _, ratio := range r.Labels.Recovery {
		recovery += ratio.Denominator
	}
	if contamination == 0 {
		r.Unsupported = append(r.Unsupported, "arm_contamination")
	}
	if recovery == 0 {
		r.Unsupported = append(r.Unsupported, "fusion_recovery")
	}
	if r.Invocations > 0 {
		se := math.Sqrt(variance)
		r.EstimatedOccurrences = &total
		r.EstimateSE = &se
	}
	r.ByKind = map[string]healthConcentration{}
	for kind, counts := range kindRecords {
		r.ByKind[kind] = healthConcentrationOf(counts)
	}
	r.Records, r.Versions = healthConcentrationOf(records), healthConcentrationOf(versions)
	r.Repeat.finish()
	r.Lifecycle.finish()
	return r, nil
}

// Runs describe retained, explicitly linked eligible turns. Boundary flags keep
// a sampled, truncated or missing predecessor/successor from becoming a claim
// about the full task history. Parallel branches are not a linear turn chain.
func healthRunDistribution(turns map[string]map[string]bool, previous map[string]string, times map[string]time.Time, incomplete map[string]bool) healthRuns {
	r := healthRuns{Lengths: map[int]int{}}
	ordered := make([]string, 0, len(turns))
	for k := range turns {
		if !incomplete[k] {
			ordered = append(ordered, k)
		}
	}
	sort.Slice(ordered, func(i, j int) bool {
		if times[ordered[i]].Equal(times[ordered[j]]) {
			return ordered[i] < ordered[j]
		}
		return times[ordered[i]].Before(times[ordered[j]])
	})
	lengths, extended := map[string]map[string]int{}, map[string]map[string]bool{}
	leftUnknown := map[string]map[string]bool{}
	hasSuccessor := map[string]bool{}
	for _, k := range ordered {
		lengths[k], extended[k], leftUnknown[k] = map[string]int{}, map[string]bool{}, map[string]bool{}
		prev := previous[k]
		linked := turns[prev] != nil && !incomplete[prev] && times[prev].Before(times[k])
		if linked {
			hasSuccessor[prev] = true
		}
		for record := range turns[k] {
			lengths[k][record] = 1
			leftUnknown[k][record] = true
			if linked {
				leftUnknown[k][record] = false
				if n := lengths[prev][record]; n > 0 {
					lengths[k][record] = n + 1
					extended[prev][record] = true
					leftUnknown[k][record] = leftUnknown[prev][record]
				}
			}
		}
	}
	for _, k := range ordered {
		for record, n := range lengths[k] {
			if !extended[k][record] {
				r.Lengths[n]++
				if leftUnknown[k][record] {
					r.UnknownStart++
				}
				if !hasSuccessor[k] {
					r.UnknownEnd++
				}
			}
		}
	}
	return r
}
