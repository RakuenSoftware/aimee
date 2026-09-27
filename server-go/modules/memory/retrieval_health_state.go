package memory

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/json"
	"errors"
	"reflect"
	"time"
)

const healthRetention = 24 * time.Hour
const healthMaxAttempts = 256

// Leaves room for parameter framing/escaping under the 1 MiB store wire cell bound.
const healthMaxStateBytes = 384 << 10

// The persistent owner stores this state in a private namespace. Public reads
// receive an aggregate, never this state or its fingerprint/sampling key.
// Snapshots enter only after authenticating the local receipt ledger; these are
// not ingestion DTOs that a model, tool caller or arbitrary plugin may author.
type healthSnapshot struct {
	Invocation       healthInvocation `json:"invocation"`
	PreparedSequence uint64           `json:"prepared_sequence,string"`
	Admitted         bool             `json:"admitted"`
	Started          bool             `json:"started"`
	Acknowledged     bool             `json:"acknowledged"`
	OutcomeUnknown   bool             `json:"outcome_unknown"`
	ResolvedUnsent   bool             `json:"resolved_unsent"`
}

type healthStoredAttempt struct {
	healthSnapshot
	Sampled        bool   `json:"sampled"`
	MetadataDigest string `json:"metadata_digest"`
	RecordCount    int    `json:"record_count"`
}

type healthJournal struct {
	Version   int                            `json:"version"`
	Namespace string                         `json:"namespace"`
	Principal string                         `json:"principal"`
	Project   string                         `json:"project"`
	Workspace string                         `json:"workspace"`
	Key       []byte                         `json:"private_key"`
	StartedAt time.Time                      `json:"started_at"`
	Attempts  map[string]healthStoredAttempt `json:"attempts"`
	Evicted   uint64                         `json:"evicted_attempts"`
	Expired   uint64                         `json:"expired_attempts"`
	// Once a prepared identity is evicted, late replay cannot insert it again
	// and inflate exposure. Older unseen snapshots are also excluded; report
	// the incomplete range rather than pretending an exact lost-attempt count.
	EvictedThrough uint64     `json:"evicted_through_prepared_sequence,string"`
	OldRangeGap    bool       `json:"old_range_incomplete"`
	GapUntil       *time.Time `json:"gap_until,omitempty"`
}

func newHealthJournal(namespace, principal, project, workspace string, now time.Time) (*healthJournal, error) {
	if namespace == "" || len(namespace) > 256 || principal == "" || len(principal) > 128 || len(project) > 1024 || len(workspace) > 1024 || now.IsZero() {
		return nil, errors.New("health namespace identity required")
	}
	key := make([]byte, 32)
	if _, err := rand.Read(key); err != nil {
		return nil, err
	}
	return &healthJournal{Version: 1, Namespace: namespace, Principal: principal, Project: project, Workspace: workspace,
		Key: key, StartedAt: now, Attempts: map[string]healthStoredAttempt{}}, nil
}

func (s healthSnapshot) stage() string {
	if s.Acknowledged {
		return "dispatched"
	}
	if s.OutcomeUnknown || s.Started {
		return "network_uncertain"
	}
	if s.ResolvedUnsent && !s.Admitted {
		return "assembled_unsent"
	}
	return "unknown_dispatch"
}

func healthSample(key []byte, attempt string, ppm int) bool {
	if ppm == 1000000 {
		return true
	}
	h := hmac.New(sha256.New, key)
	raw, _ := json.Marshal([]string{"memory-health-invocation-sampling-v1", attempt})
	_, _ = h.Write(raw)
	// Compare in the 53-bit exactly representable range. The rounding error is
	// below 2^-53; records in one invocation share this single decision.
	v := binary.BigEndian.Uint64(h.Sum(nil)[:8]) >> 11
	return float64(v)/float64(uint64(1)<<53) < float64(ppm)/1000000
}

func (s *healthJournal) apply(observation healthSnapshot, now time.Time) error {
	e := observation.Invocation
	if s.Version != 1 || len(s.Key) != 32 || s.Attempts == nil || s.Namespace == "" || s.Principal == "" {
		return errors.New("invalid health journal")
	}
	if e.Namespace != s.Namespace || e.Principal != s.Principal || e.Project != s.Project || e.Workspace != s.Workspace {
		return errors.New("health observation belongs to another namespace")
	}
	if observation.PreparedSequence == 0 || e.Attempt == "" || e.Binding == "" || e.At.IsZero() || e.At.After(now) ||
		e.SamplePPM <= 0 || e.SamplePPM > 1000000 || e.SamplingEpoch == "" || len(e.Records) > 256 ||
		(observation.Started || observation.Acknowledged || observation.OutcomeUnknown) && !observation.Admitted ||
		observation.ResolvedUnsent && observation.Admitted {
		return errors.New("invalid receipt-backed health observation")
	}
	if raw, err := json.Marshal(e); err != nil || len(raw) > 65536 {
		return errors.New("health metadata exceeds invocation bound")
	}
	unique := map[string]bool{}
	for _, record := range e.Records {
		if record.RecordID == "" || record.VersionID == "" {
			return errors.New("health record identity required")
		}
		unique[record.RecordID] = true
	}
	// A serving stage is derived from authenticated observations, not a field
	// carried by the selected-record metadata.
	e.Stage = ""
	observation.Invocation = e
	digest := releaseDigest(e)
	old, exists := s.Attempts[e.Attempt]
	if exists && (old.MetadataDigest != digest || old.PreparedSequence != observation.PreparedSequence) {
		return errors.New("health attempt changed its immutable selection")
	}
	if exists && (old.ResolvedUnsent && observation.Admitted || observation.ResolvedUnsent && old.Admitted) {
		return errors.New("receipt contradicts resolved unsent state")
	}
	cutoff := now.Add(-healthRetention)
	for id, entry := range s.Attempts {
		if entry.Invocation.At.Before(cutoff) {
			delete(s.Attempts, id)
			s.Expired++
		}
	}
	if e.At.Before(cutoff) {
		return nil
	}
	if !exists && observation.PreparedSequence <= s.EvictedThrough {
		s.markGap(e.At, now)
		return nil
	}
	if exists {
		old.Admitted = old.Admitted || observation.Admitted
		old.Started = old.Started || observation.Started
		old.Acknowledged = old.Acknowledged || observation.Acknowledged
		old.OutcomeUnknown = old.OutcomeUnknown || observation.OutcomeUnknown
		old.ResolvedUnsent = old.ResolvedUnsent || observation.ResolvedUnsent
	} else {
		old = healthStoredAttempt{healthSnapshot: observation, Sampled: healthSample(s.Key, e.Attempt, e.SamplePPM), MetadataDigest: digest, RecordCount: len(unique)}
		if !old.Sampled {
			old.Invocation.Records = nil
			old.Invocation.Labels = nil
		}
	}
	s.Attempts[e.Attempt] = old
	for {
		raw, err := json.Marshal(s)
		if err != nil {
			return err
		}
		if len(s.Attempts) <= healthMaxAttempts && len(raw) <= healthMaxStateBytes {
			break
		}
		oldest := ""
		for id, entry := range s.Attempts {
			if oldest == "" || entry.PreparedSequence < s.Attempts[oldest].PreparedSequence {
				oldest = id
			}
		}
		if oldest == "" {
			return errors.New("health namespace metadata exceeds bound")
		}
		if seq := s.Attempts[oldest].PreparedSequence; seq > s.EvictedThrough {
			s.EvictedThrough = seq
		}
		s.markGap(s.Attempts[oldest].Invocation.At, now)
		delete(s.Attempts, oldest)
		s.Evicted++
	}
	return nil
}

// A prior capacity loss does not poison every future window forever. Legacy
// journals without a loss boundary remain incomplete until a conservative new
// boundary has aged out; no historical coverage is invented during migration.
func (s *healthJournal) markGap(at, now time.Time) {
	through := at.Add(time.Nanosecond)
	if s.OldRangeGap && s.GapUntil == nil {
		through = now.Add(time.Nanosecond)
	}
	if s.GapUntil == nil || through.After(*s.GapUntil) {
		s.GapUntil = &through
	}
	s.OldRangeGap = true
}

type healthWindow struct {
	SamplingProbabilities map[int]int    `json:"sampling_probability_ppm_counts"`
	MetadataGaps          map[string]int `json:"metadata_gaps"`
	Metrics               healthMetrics  `json:"sample_metrics"`
	Attempts              map[string]int `json:"exact_retained_attempts_by_stage"`
	Records               map[string]int `json:"exact_retained_record_occurrences_by_stage"`
	Sampled               int            `json:"sampled_invocations"`
	NotSampled            int            `json:"unsampled_invocations"`
	Complete              bool           `json:"window_complete"`
	CoverageStart         time.Time      `json:"coverage_start"`
	RetentionSeconds      int64          `json:"retention_seconds"`
	Evicted               uint64         `json:"capacity_evictions_since_start"`
	Expired               uint64         `json:"retention_expirations_since_start"`
	Gap                   bool           `json:"old_range_incomplete"`
}

func (s *healthJournal) report(p healthPopulation, now time.Time) (healthWindow, error) {
	r := healthWindow{SamplingProbabilities: map[int]int{}, MetadataGaps: map[string]int{}, Attempts: map[string]int{}, Records: map[string]int{}, CoverageStart: s.StartedAt,
		RetentionSeconds: int64(healthRetention / time.Second), Evicted: s.Evicted, Expired: s.Expired, Gap: s.OldRangeGap && (s.GapUntil == nil || p.From.Before(*s.GapUntil))}
	if p.Namespace != s.Namespace || p.Principal != s.Principal || p.Project != s.Project || p.Workspace != s.Workspace {
		return healthWindow{}, errors.New("health report belongs to another namespace")
	}
	// This is coverage of this owner's collector, not of all serving surfaces.
	r.Complete = !p.From.Before(s.StartedAt) && !p.From.Before(now.Add(-healthRetention)) && !p.Until.After(now) && !r.Gap
	var sampled []healthInvocation
	for _, entry := range s.Attempts {
		e := entry.Invocation
		e.Stage = entry.stage()
		allStages := p
		allStages.Stage = e.Stage
		if !allStages.includes(e) || e.At.Before(now.Add(-healthRetention)) {
			continue
		}
		r.Attempts[e.Stage]++
		for _, gap := range e.MetadataGaps {
			r.MetadataGaps[gap]++
		}
		r.Records[e.Stage] += entry.RecordCount
		if e.Stage != p.Stage {
			continue
		}
		r.SamplingProbabilities[e.SamplePPM]++
		if entry.Sampled {
			sampled = append(sampled, e)
			r.Sampled++
		} else {
			r.NotSampled++
		}
	}
	var err error
	r.Metrics, err = aggregateHealth(sampled, p)
	return r, err
}

// Round-trip validation belongs at the persistence boundary. A schema change
// or corrupted private state cannot silently become an empty healthy window.
func decodeHealthJournal(raw []byte) (*healthJournal, error) {
	var s healthJournal
	if len(raw) == 0 || len(raw) > healthMaxStateBytes || json.Unmarshal(raw, &s) != nil || s.Version != 1 || len(s.Key) != 32 || s.Attempts == nil || len(s.Attempts) > healthMaxAttempts ||
		s.Namespace == "" || len(s.Namespace) > 256 || s.Principal == "" || len(s.Principal) > 128 || len(s.Project) > 1024 || len(s.Workspace) > 1024 || s.StartedAt.IsZero() ||
		s.Evicted > 0 && (!s.OldRangeGap || s.EvictedThrough == 0) {
		return nil, errors.New("invalid persisted health journal")
	}
	canonical, _ := json.Marshal(s)
	a, err := receiptJSONTree(raw)
	if err != nil {
		return nil, err
	}
	b, err := receiptJSONTree(canonical)
	if err != nil || !reflect.DeepEqual(a, b) {
		return nil, errors.New("ambiguous health journal")
	}
	for id, entry := range s.Attempts {
		e := entry.Invocation
		if id == "" || id != e.Attempt || e.Namespace != s.Namespace || e.Principal != s.Principal || e.Project != s.Project || e.Workspace != s.Workspace ||
			e.Binding == "" || e.At.IsZero() || e.Stage != "" || e.SamplePPM <= 0 || e.SamplePPM > 1000000 || e.SamplingEpoch == "" ||
			entry.PreparedSequence == 0 || !receiptDigestValid(entry.MetadataDigest) || entry.RecordCount < 0 || entry.RecordCount > 256 || len(e.Records) > 256 ||
			entry.Sampled != healthSample(s.Key, id, e.SamplePPM) || !entry.Sampled && (e.Records != nil || e.Labels != nil) ||
			(entry.Started || entry.Acknowledged || entry.OutcomeUnknown) && !entry.Admitted || entry.ResolvedUnsent && entry.Admitted {
			return nil, errors.New("invalid persisted health attempt")
		}
		if entry.Sampled {
			unique := map[string]bool{}
			for _, record := range e.Records {
				if record.RecordID == "" || record.VersionID == "" {
					return nil, errors.New("invalid persisted health record")
				}
				unique[record.RecordID] = true
			}
			if len(unique) != entry.RecordCount || entry.MetadataDigest != releaseDigest(e) {
				return nil, errors.New("persisted health selection changed")
			}
		}
	}
	return &s, nil
}
