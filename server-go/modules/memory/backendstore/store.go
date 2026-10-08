// Package backendstore implements the durable compatibility store owned by an
// external engine adapter. It never imports native memory or PostgreSQL.
package backendstore

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	memory "github.com/JBailes/aimee/server-go/memory"
	"golang.org/x/sys/unix"
	"io"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"
)

var ErrErasedAdmission = fmt.Errorf("%w: retained erasure admission", ErrConflict)

var ErrConflict = errors.New("memory: backend revision conflict")
var ErrReplayUnavailable = errors.New("memory: backend replay unavailable")
var ErrIdempotency = errors.New("memory: backend idempotency conflict")

const MaxSnapshot = 64 << 20

type Entry struct {
	Record  memory.Record   `json:"record"`
	Retired bool            `json:"retired"`
	History []memory.Record `json:"history"`
}
type Receipt struct {
	Digest  string        `json:"digest"`
	Record  memory.Record `json:"record"`
	Deleted bool          `json:"deleted"`
}
type Snapshot struct {
	ErasedPayloads  map[string]bool    `json:"erased_payloads,omitempty"`
	ErasureRequests map[string]string  `json:"erasure_requests,omitempty"`
	Generation      int64              `json:"generation,omitempty"`
	SubjectEpochs   map[string]int64   `json:"subject_epochs,omitempty"`
	Guards          map[string]int64   `json:"guards,omitempty"`
	ErasedSubjects  map[string]bool    `json:"erased_subjects,omitempty"`
	Version         int                `json:"version"`
	Owner           string             `json:"owner"`
	Next            int64              `json:"next"`
	Entries         map[int64]Entry    `json:"entries"`
	Erased          map[int64]bool     `json:"erased"`
	Receipts        map[string]Receipt `json:"receipts"`
}
type Store struct{ dir, owner string }

func New(dir, owner string) (*Store, error) {
	if dir == "" || !filepath.IsAbs(dir) || !(&memory.MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: "1", RecordRevision: "1"}).ValidFor(1) {
		return nil, memory.ErrUnavailable
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.Mode().Perm()&0077 != 0 {
		return nil, memory.ErrUnavailable
	}
	s := &Store{dir, owner}
	err = s.transaction(context.Background(), true, func(*Snapshot) error { return nil })
	return s, err
}
func (s *Store) Namespace(scope memory.Scope) string {
	return s.owner + "/" + scope.Type + "/" + scope.Value
}
func (s *Store) transaction(ctx context.Context, write bool, apply func(*Snapshot) error) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	fd, err := unix.Open(filepath.Join(s.dir, "lock"), unix.O_CREAT|unix.O_RDWR|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return err
	}
	defer unix.Close(fd)
	for {
		err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB)
		if err == nil {
			break
		}
		if err != unix.EWOULDBLOCK && err != unix.EAGAIN {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(5 * time.Millisecond):
		}
	}
	defer unix.Flock(fd, unix.LOCK_UN)
	epoch, err := collectionEpoch()
	if err != nil {
		return err
	}
	snapshot := Snapshot{Version: 1, Owner: epoch, Next: 1, Entries: map[int64]Entry{}, Erased: map[int64]bool{}, Receipts: map[string]Receipt{}}
	path := filepath.Join(s.dir, "records.json")
	file, err := os.OpenFile(path, os.O_RDONLY|unix.O_NOFOLLOW, 0)
	if err == nil {
		info, e := file.Stat()
		if e != nil || info.Mode().Perm()&0077 != 0 || info.Size() > MaxSnapshot {
			file.Close()
			return memory.ErrUnavailable
		}
		raw, e := io.ReadAll(io.LimitReader(file, MaxSnapshot+1))
		file.Close()
		if e != nil {
			return e
		}
		decoder := json.NewDecoder(strings.NewReader(string(raw)))
		decoder.DisallowUnknownFields()
		if decoder.Decode(&snapshot) != nil || decoder.Decode(&struct{}{}) != io.EOF || validate(snapshot) != nil {
			return memory.ErrUnavailable
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	if snapshot.ErasedSubjects == nil {
		snapshot.ErasedSubjects = map[string]bool{}
	}
	if snapshot.SubjectEpochs == nil {
		snapshot.SubjectEpochs = map[string]int64{}
	}
	if snapshot.ErasureRequests == nil {
		snapshot.ErasureRequests = map[string]string{}
	}
	if snapshot.Guards == nil {
		snapshot.Guards = map[string]int64{}
	}
	// Retained erasure control state is stored separately from replaceable
	// catalog backups. Restoring records.json cannot restore erased payloads.
	ledger := Snapshot{Erased: map[int64]bool{}, ErasedSubjects: map[string]bool{}}
	ledgerPath := filepath.Join(s.dir, "erasures.json")
	ledgerFile, ledgerErr := os.OpenFile(ledgerPath, os.O_RDONLY|unix.O_NOFOLLOW, 0)
	if ledgerErr == nil {
		info, e := ledgerFile.Stat()
		if e != nil || info.Mode().Perm()&0077 != 0 || info.Size() > MaxSnapshot {
			ledgerFile.Close()
			return memory.ErrUnavailable
		}
		raw, e := io.ReadAll(io.LimitReader(ledgerFile, MaxSnapshot+1))
		ledgerFile.Close()
		if e != nil || len(raw) > MaxSnapshot || json.Unmarshal(raw, &ledger) != nil || ledger.Erased == nil || ledger.ErasedSubjects == nil {
			return memory.ErrUnavailable
		}
	} else if !os.IsNotExist(ledgerErr) {
		return ledgerErr
	}
	for id := range ledger.Erased {
		snapshot.Erased[id] = true
		if id >= snapshot.Next {
			snapshot.Next = id + 1
		}
	}
	if snapshot.ErasedPayloads == nil {
		snapshot.ErasedPayloads = map[string]bool{}
	}
	for key := range ledger.ErasedPayloads {
		snapshot.ErasedPayloads[key] = true
	}
	snapshot.Generation = max(snapshot.Generation, ledger.Generation)
	for id, digest := range ledger.ErasureRequests {
		if prior, ok := snapshot.ErasureRequests[id]; ok && prior != digest {
			return memory.ErrUnavailable
		}
		snapshot.ErasureRequests[id] = digest
	}
	for subject, epoch := range ledger.SubjectEpochs {
		if epoch > snapshot.SubjectEpochs[subject] {
			snapshot.SubjectEpochs[subject] = epoch
		}
	}
	for subject := range ledger.ErasedSubjects {
		snapshot.ErasedSubjects[subject] = true
	}
	// Upgrade pre-epoch erasure markers conservatively. Legacy payloads lack
	// this admission epoch and remain erased; fresh host admissions use epoch one.
	for marker := range snapshot.ErasedSubjects {
		if snapshot.SubjectEpochs[marker] == 0 {
			snapshot.SubjectEpochs[marker] = 1
		}
	}
	before := canonicalDigest(snapshot)
	for id, entry := range snapshot.Entries {
		erased := snapshot.Erased[id]
		for _, r := range append(entry.History, entry.Record) {
			erased = erased || erasedRecord(&snapshot, r)
		}
		if erased {
			delete(snapshot.Entries, id)
			snapshot.Erased[id] = true
		}
	}
	for key, receipt := range snapshot.Receipts {
		if snapshot.Erased[receipt.Record.ID] {
			delete(snapshot.Receipts, key)
		}
	}
	if err = apply(&snapshot); err != nil {
		return err
	}
	if !write {
		return ctx.Err()
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if canonicalDigest(snapshot) != before {
		if snapshot.Generation == math.MaxInt64 {
			return memory.ErrCapacity
		}
		snapshot.Generation++
	}
	raw, err := json.Marshal(snapshot)
	if err != nil {
		return err
	}
	if len(raw) > MaxSnapshot {
		return memory.ErrCapacity
	}
	ledgerRaw, err := json.Marshal(Snapshot{Erased: snapshot.Erased, ErasedSubjects: snapshot.ErasedSubjects, SubjectEpochs: snapshot.SubjectEpochs, Generation: snapshot.Generation, ErasureRequests: snapshot.ErasureRequests, ErasedPayloads: snapshot.ErasedPayloads})
	if err != nil {
		return err
	}
	if err = s.atomicWrite(ledgerPath, ledgerRaw); err != nil {
		return err
	}
	return s.atomicWrite(path, raw)
}
func (s *Store) atomicWrite(path string, raw []byte) error {
	temp, err := os.CreateTemp(s.dir, ".commit-")
	if err != nil {
		return err
	}
	defer os.Remove(temp.Name())
	defer temp.Close()
	if _, err = temp.Write(raw); err != nil {
		return err
	}
	if err = temp.Sync(); err != nil {
		return err
	}
	if err = temp.Close(); err != nil {
		return err
	}
	if err = os.Rename(temp.Name(), path); err != nil {
		return err
	}
	directory, err := os.Open(s.dir)
	if err != nil {
		return err
	}
	defer directory.Close()
	return directory.Sync()
}

func validate(s Snapshot) error {
	if s.Generation < 0 || s.Generation == math.MaxInt64 {
		return memory.ErrUnavailable
	}
	for marker, epoch := range s.SubjectEpochs {
		if !strings.HasPrefix(marker, "sha256:") || len(marker) != 71 || epoch <= 0 {
			return memory.ErrUnavailable
		}
	}
	if s.Version != 1 || s.Next <= 0 || s.Entries == nil || s.Erased == nil || s.Receipts == nil {
		return memory.ErrUnavailable
	}
	if !(&memory.MemoryRecordVersion{SchemaVersion: 1, OwnerID: s.Owner, RecordID: "1", RecordRevision: "1"}).ValidFor(1) {
		return memory.ErrUnavailable
	}
	used := map[string]bool{}
	for id, e := range s.Entries {
		r := e.Record
		scope, err := r.Scope.Canonical()
		if err != nil || scope != r.Scope || id != r.ID || id <= 0 || id >= s.Next || s.Erased[id] || !r.Version.ValidFor(id) || r.Version.OwnerID != s.Owner {
			return memory.ErrUnavailable
		}
		if !validRecord(r) {
			return memory.ErrUnavailable
		}
		priorRevision := int64(0)
		if !e.Retired {
			keyRaw, _ := json.Marshal([]string{r.Scope.Type, r.Scope.Value, r.Kind, r.Key})
			key := string(keyRaw)
			if used[key] {
				return memory.ErrUnavailable
			}
			used[key] = true
		}
		for _, h := range e.History {
			if !validRecord(h) || !h.Version.ValidFor(id) {
				return memory.ErrUnavailable
			}
			revision, parseErr := strconv.ParseInt(h.Version.RecordRevision, 10, 64)
			if parseErr != nil || revision <= priorRevision {
				return memory.ErrUnavailable
			}
			priorRevision = revision
			if h.ID != id || h.Scope != r.Scope || !h.Version.ValidFor(id) || h.Version.OwnerID != s.Owner {
				return memory.ErrUnavailable
			}
		}
		currentRevision, parseErr := strconv.ParseInt(r.Version.RecordRevision, 10, 64)
		if parseErr != nil || currentRevision <= priorRevision {
			return memory.ErrUnavailable
		}
	}
	for _, receipt := range s.Receipts {
		r := receipt.Record
		entry, exists := s.Entries[r.ID]
		if !exists || !validRecord(r) || !r.Version.ValidFor(r.ID) || r.Version.OwnerID != s.Owner || entry.Record.Scope != r.Scope || receipt.Digest == "" {
			return memory.ErrUnavailable
		}
	}
	return nil
}
func validRecord(r memory.Record) bool {
	return r.Kind != "" && r.Key != "" && len(r.Content) <= 512*1024 && !math.IsNaN(r.Confidence) && !math.IsInf(r.Confidence, 0) && r.Confidence >= 0 && r.Confidence <= 1 && (len(r.Authorship) == 0 || json.Valid(r.Authorship))
}
func (s *Store) Get(ctx context.Context, scope memory.Scope, id int64) (out memory.Record, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		e, ok := state.Entries[id]
		if !ok || e.Retired || !eligibleRecord(e.Record) || e.Record.Scope != scope {
			return memory.ErrNotFound
		}
		out = e.Record
		return nil
	})
	return
}
func (s *Store) GetVersion(ctx context.Context, scope memory.Scope, id int64, version *memory.MemoryRecordVersion) (out memory.Record, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		e, ok := state.Entries[id]
		if !ok || e.Record.Scope != scope {
			return memory.ErrNotFound
		}
		for _, r := range append(e.History, e.Record) {
			if version != nil && r.Version != nil && *r.Version == *version {
				out = r
				out.Historical = e.Retired || *version != *e.Record.Version
				return nil
			}
		}
		return memory.ErrNotFound
	})
	return
}
func (s *Store) List(ctx context.Context, scope memory.Scope, after int64, limit int) (out []memory.Record, err error) {
	if limit < 1 || limit > 100000 {
		return nil, memory.ErrCapacity
	}
	out = []memory.Record{}
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		for _, e := range state.Entries {
			if !e.Retired && eligibleRecord(e.Record) && e.Record.Scope == scope && e.Record.ID > after {
				out = append(out, e.Record)
			}
		}
		sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
		if len(out) > limit {
			out = out[:limit]
		}
		return nil
	})
	return
}
func (s *Store) Search(ctx context.Context, scope memory.Scope, query, kind, tier string, limit int) (out []memory.Record, err error) {
	if limit < 1 || limit > 100000 {
		return nil, memory.ErrCapacity
	}
	out = []memory.Record{}
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		for _, e := range state.Entries {
			r := e.Record
			if !e.Retired && eligibleRecord(r) && r.Scope == scope && (kind == "" || kind == r.Kind) && (tier == "" || tier == r.Tier) && (query == "" || strings.Contains(strings.ToLower(r.Key+" "+r.Content), strings.ToLower(query))) {
				out = append(out, r)
			}
		}
		sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
		if len(out) > limit {
			out = out[:limit]
		}
		return nil
	})
	return
}

type Mutation struct {
	Scope       memory.Scope
	Record      memory.Record
	Expected    *memory.MemoryRecordVersion
	Delete      bool
	Key, Digest string
	Admit       func(*memory.Record) error
}

func (s *Store) Mutate(ctx context.Context, m Mutation) (out memory.Record, deleted bool, err error) {
	err = s.transaction(ctx, true, func(state *Snapshot) error {
		if guarded(state) {
			return memory.ErrUnavailable
		}
		if m.Key != "" {
			if receipt, ok := state.Receipts[m.Key]; ok {
				if receipt.Digest != m.Digest {
					return ErrIdempotency
				}
				current, exists := state.Entries[receipt.Record.ID]
				if !exists || (current.Retired && !receipt.Deleted) || (!current.Retired && receipt.Deleted) || current.Record.Version == nil || receipt.Record.Version == nil || *current.Record.Version != *receipt.Record.Version {
					return ErrReplayUnavailable
				}
				out, deleted = current.Record, receipt.Deleted
				return nil
			}
		}
		scope, e := m.Scope.Canonical()
		if e != nil || scope != m.Scope {
			return memory.ErrClientRequest
		}
		r := m.Record
		if erasedRecord(state, r) {
			return ErrErasedAdmission
		}
		r.Scope = scope
		id := r.ID
		if id == 0 && !m.Delete {
			for _, entry := range state.Entries {
				if entry.Record.Scope == scope && entry.Record.Kind == r.Kind && entry.Record.Key == r.Key {
					id = entry.Record.ID
					break
				}
			}
		}
		previous, exists := state.Entries[id]
		if id != 0 && (!exists || previous.Record.Scope != scope) {
			return memory.ErrNotFound
		}
		if m.Expected != nil && (!exists || previous.Record.Version == nil || *m.Expected != *previous.Record.Version) {
			return ErrConflict
		}
		if m.Admit != nil {
			var old *memory.Record
			if exists {
				copy := previous.Record
				copy.Historical = previous.Retired
				old = &copy
			}
			if e = m.Admit(old); e != nil {
				return e
			}
		}
		if m.Delete {
			if !exists || previous.Retired {
				return memory.ErrNotFound
			}
			r = previous.Record
			previous.History = append(previous.History, r)
			previous.Retired = true
			deleted = true
		} else {
			if r.Kind == "" || r.Key == "" || len(r.Content) > 512*1024 || math.IsNaN(r.Confidence) || math.IsInf(r.Confidence, 0) || r.Confidence < 0 || r.Confidence > 1 {
				return memory.ErrClientRequest
			}
			if !exists {
				if state.Next == math.MaxInt64 {
					return memory.ErrCapacity
				}
				id = state.Next
				state.Next++
				previous = Entry{}
			} else {
				previous.History = append(previous.History, previous.Record)
			}
			previous.Retired = false
		}
		revision := int64(1)
		if exists {
			revision, _ = strconv.ParseInt(previous.Record.Version.RecordRevision, 10, 64)
			if revision == math.MaxInt64 {
				return memory.ErrCapacity
			}
			revision++
		}
		r.ID = id
		r.Historical = false
		r.Version = &memory.MemoryRecordVersion{SchemaVersion: 1, OwnerID: state.Owner, RecordID: strconv.FormatInt(id, 10), RecordRevision: strconv.FormatInt(revision, 10)}
		previous.Record = r
		state.Entries[id] = previous
		out = r
		if m.Key != "" {
			state.Receipts[m.Key] = Receipt{m.Digest, out, deleted}
		}
		return nil
	})
	return
}
func (s *Store) Put(ctx context.Context, scope memory.Scope, r memory.Record) (memory.Record, error) {
	out, _, err := s.Mutate(ctx, Mutation{Scope: scope, Record: r})
	return out, err
}
func (s *Store) Delete(ctx context.Context, scope memory.Scope, id int64) (bool, error) {
	_, deleted, err := s.Mutate(ctx, Mutation{Scope: scope, Record: memory.Record{ID: id}, Delete: true})
	if errors.Is(err, memory.ErrNotFound) {
		return false, nil
	}
	return deleted, err
}
func (s *Store) Export(ctx context.Context) (raw []byte, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		exported := *state
		exported.Guards = nil
		raw, err = json.Marshal(exported)
		return err
	})
	return
}
func (s *Store) Import(ctx context.Context, raw []byte) error {
	if len(raw) > MaxSnapshot {
		return memory.ErrCapacity
	}
	var incoming Snapshot
	decoder := json.NewDecoder(strings.NewReader(string(raw)))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&incoming) != nil || decoder.Decode(&struct{}{}) != io.EOF || validate(incoming) != nil {
		return memory.ErrClientRequest
	}
	return s.transaction(ctx, true, func(state *Snapshot) error {
		if guarded(state) {
			return memory.ErrUnavailable
		}
		// Migration is explicit into an empty engine store, never overwrite a live
		// catalog. Retained erasure markers always veto stale backup resurrection.
		if len(state.Entries) != 0 || len(state.Receipts) != 0 {
			return ErrConflict
		}
		for id := range state.Erased {
			if _, ok := incoming.Entries[id]; ok {
				return ErrConflict
			}
			incoming.Erased[id] = true
		}
		if incoming.ErasedSubjects == nil {
			incoming.ErasedSubjects = map[string]bool{}
		}
		if incoming.SubjectEpochs == nil {
			incoming.SubjectEpochs = map[string]int64{}
		}
		for subject, epoch := range state.SubjectEpochs {
			if epoch > incoming.SubjectEpochs[subject] {
				incoming.SubjectEpochs[subject] = epoch
			}
		}
		if incoming.ErasureRequests == nil {
			incoming.ErasureRequests = map[string]string{}
		}
		for id, digest := range state.ErasureRequests {
			if prior, ok := incoming.ErasureRequests[id]; ok && prior != digest {
				return ErrConflict
			}
			incoming.ErasureRequests[id] = digest
		}
		if incoming.ErasedPayloads == nil {
			incoming.ErasedPayloads = map[string]bool{}
		}
		for key := range state.ErasedPayloads {
			incoming.ErasedPayloads[key] = true
		}
		incoming.Guards = map[string]int64{}
		incoming.Generation = max(incoming.Generation, state.Generation)
		for subject := range state.ErasedSubjects {
			incoming.ErasedSubjects[subject] = true
		}
		for _, entry := range incoming.Entries {
			for _, record := range append(entry.History, entry.Record) {
				if erasedRecord(&incoming, record) {
					return ErrConflict
				}
			}
		}
		*state = incoming
		return nil
	})
}
func (s *Store) Erase(ctx context.Context, scope memory.Scope, ids []int64) (count int, err error) {
	err = s.transaction(ctx, true, func(state *Snapshot) error {
		if guarded(state) {
			return memory.ErrUnavailable
		}
		selected := map[int64]bool{}
		for _, id := range ids {
			selected[id] = true
		}
		for id, e := range state.Entries {
			if e.Record.Scope == scope && (len(ids) == 0 || selected[id]) {
				delete(state.Entries, id)
				state.Erased[id] = true
				count++
			}
		}
		for key, r := range state.Receipts {
			if state.Erased[r.Record.ID] {
				delete(state.Receipts, key)
			}
		}
		return nil
	})
	return
}

func erasedAuthorship(state *Snapshot, raw json.RawMessage) bool {
	var fields map[string]any
	if len(raw) == 0 {
		return false
	}
	if json.Unmarshal(raw, &fields) != nil {
		return true
	}
	for _, key := range []string{"principal", "session_id"} {
		if value, ok := fields[key].(string); ok && value != "" {
			if state.ErasedSubjects[erasureMarker(value)] {
				epoch, ok := fields["erasure_epoch"].(string)
				expected := state.SubjectEpochs[erasureMarker(value)]
				if key != "principal" || !ok || expected == 0 || epoch != strconv.FormatInt(expected, 10) {
					return true
				}
			}
			if key == "session_id" {
				digest := sha256.Sum256([]byte(value))
				if state.ErasedSubjects["sha256:"+hex.EncodeToString(digest[:])] {
					return true
				}
			}
		}
	}
	return false
}

// EraseSubject records the retained intent and removes current/history/retry
// payloads together. Replayed imports and late writes cannot resurrect them.
func (s *Store) EraseSubject(ctx context.Context, subject string, sessions []string) (int, error) {
	return s.EraseSubjectRequest(ctx, subject, sessions, "")
}
func (s *Store) EraseSubjectRequest(ctx context.Context, subject string, sessions []string, requestID string) (count int, err error) {
	if subject == "" || len(subject) > 1024 {
		return 0, memory.ErrClientRequest
	}
	err = s.transaction(ctx, true, func(state *Snapshot) error {
		if guarded(state) {
			return memory.ErrUnavailable
		}
		if requestID != "" {
			markers := []string{erasureMarker(subject)}
			for _, session := range sessions {
				markers = append(markers, erasureMarker(session))
			}
			sort.Strings(markers)
			raw, _ := json.Marshal(markers)
			sum := sha256.Sum256(raw)
			digest := hex.EncodeToString(sum[:])
			id := erasureMarker(requestID)
			if prior, ok := state.ErasureRequests[id]; ok {
				if prior != digest {
					return ErrIdempotency
				}
				return nil
			}
			state.ErasureRequests[id] = digest
		}
		marker := erasureMarker(subject)
		if state.SubjectEpochs[marker] == math.MaxInt64 {
			return memory.ErrCapacity
		}
		state.SubjectEpochs[marker]++
		state.ErasedSubjects[marker] = true
		for _, session := range sessions {
			if len(session) > 256 {
				return memory.ErrClientRequest
			}
			state.ErasedSubjects[erasureMarker(session)] = true
		}
		for id, e := range state.Entries {
			for _, r := range append(e.History, e.Record) {
				if erasedAuthorship(state, r.Authorship) {
					delete(state.Entries, id)
					state.Erased[id] = true
					count++
					break
				}
			}
		}
		for key, r := range state.Receipts {
			if state.Erased[r.Record.ID] {
				delete(state.Receipts, key)
			}
		}
		return nil
	})
	return
}

func (s *Store) ExportScope(ctx context.Context, scope memory.Scope) (raw []byte, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		filtered := *state
		filtered.Guards = nil
		filtered.Entries = map[int64]Entry{}
		filtered.Receipts = map[string]Receipt{}
		for id, entry := range state.Entries {
			if entry.Record.Scope == scope {
				filtered.Entries[id] = entry
			}
		}
		for key, receipt := range state.Receipts {
			if receipt.Record.Scope == scope {
				filtered.Receipts[key] = receipt
			}
		}
		raw, err = json.Marshal(filtered)
		return err
	})
	return
}
func (s *Store) ImportScope(ctx context.Context, scope memory.Scope, raw []byte) error {
	var state Snapshot
	if json.Unmarshal(raw, &state) != nil {
		return memory.ErrClientRequest
	}
	for _, entry := range state.Entries {
		if entry.Record.Scope != scope {
			return memory.ErrClientRequest
		}
	}
	for _, receipt := range state.Receipts {
		if receipt.Record.Scope != scope {
			return memory.ErrClientRequest
		}
	}
	return s.Import(ctx, raw)
}

func erasureMarker(value string) string {
	if strings.HasPrefix(value, "sha256:") && len(value) == 71 {
		if _, err := hex.DecodeString(strings.TrimPrefix(value, "sha256:")); err == nil {
			return value
		}
	}
	digest := sha256.Sum256([]byte(value))
	return "sha256:" + hex.EncodeToString(digest[:])
}

func collectionEpoch() (string, error) {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	raw[6] = (raw[6] & 0x0f) | 0x40
	raw[8] = (raw[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", raw[:4], raw[4:6], raw[6:8], raw[8:10], raw[10:]), nil
}

// AdmissionEpoch is captured by the authenticated host before a write. It is
// never accepted from wire arguments. Erasure racing that write invalidates it.
func (s *Store) AdmissionEpoch(ctx context.Context, principal string) (epoch int64, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		epoch = state.SubjectEpochs[erasureMarker(principal)]
		return nil
	})
	return
}

// The deadline bounds dispatch, not storage protection. A paused sender may
// resume after expiry; only authenticated completion releases its barrier.
func guarded(state *Snapshot) bool { return len(state.Guards) > 0 }

// Observe binds even an empty result to a collection generation. Revalidate
// compares under the same process-shared lock used by every canonical mutation.
func (s *Store) Observe(ctx context.Context) (version memory.MemoryRecordVersion, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		version = memory.MemoryRecordVersion{SchemaVersion: 1, OwnerID: state.Owner, RecordID: "1", RecordRevision: strconv.FormatInt(state.Generation+1, 10)}
		return nil
	})
	return
}
func (s *Store) Revalidate(ctx context.Context, check, guard string, collection *memory.MemoryRecordVersion, scopes []memory.Scope, versions []memory.MemoryRecordVersion, boundaries ...string) (eligible bool, err error) {
	err = s.transaction(ctx, guard != "", func(state *Snapshot) error {
		if guard == "release" {
			delete(state.Guards, check)
			eligible = true
			return nil
		}
		for _, value := range boundaries {
			if value != "" {
				stamp, e := time.Parse(time.RFC3339Nano, value)
				now := time.Now()
				if guard == "acquire" {
					now = now.Add(5 * time.Second)
				}
				if e != nil || !now.Before(stamp) {
					return nil
				}
			}
		}
		if collection != nil && (collection.OwnerID != state.Owner || collection.RecordRevision != strconv.FormatInt(state.Generation+1, 10)) {
			return nil
		}
		for _, v := range versions {
			id, e := strconv.ParseInt(v.RecordID, 10, 64)
			if e != nil {
				return memory.ErrClientRequest
			}
			entry, ok := state.Entries[id]
			if !ok || entry.Retired || !eligibleRecord(entry.Record) || entry.Record.Version == nil || *entry.Record.Version != v {
				return nil
			}
			admitted := false
			for _, scope := range scopes {
				admitted = admitted || entry.Record.Scope == scope
			}
			if !admitted {
				return nil
			}
		}
		eligible = true
		if guard == "acquire" {
			if _, exists := state.Guards[check]; exists {
				return memory.ErrUnavailable
			}
			state.Guards[check] = time.Now().Add(5 * time.Second).UnixMilli()
		}
		return nil
	})
	return
}

func (s *Store) Candidates(ctx context.Context, scope memory.Scope, query, kind, tier string, limit int) (out []memory.Record, err error) {
	if limit < 1 || limit > 100000 {
		return nil, memory.ErrCapacity
	}
	out = []memory.Record{}
	terms := strings.FieldsFunc(strings.ToLower(query), func(r rune) bool { return !unicode.IsLetter(r) && !unicode.IsNumber(r) })
	score := func(r memory.Record) int {
		text := strings.ToLower(r.Key + " " + r.Content)
		n := 0
		for _, term := range terms {
			if strings.Contains(text, term) {
				n++
			}
		}
		return n
	}
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		for _, entry := range state.Entries {
			r := entry.Record
			if !entry.Retired && eligibleRecord(r) && r.Scope == scope && (kind == "" || r.Kind == kind) && (tier == "" || r.Tier == tier) {
				out = append(out, r)
			}
		}
		sort.Slice(out, func(i, j int) bool {
			a, b := score(out[i]), score(out[j])
			if a != b {
				return a > b
			}
			return out[i].ID > out[j].ID
		})
		if len(out) > limit {
			out = out[:limit]
		}
		return nil
	})
	return
}

func canonicalDigest(state Snapshot) [32]byte {
	state.Guards = nil
	state.Generation = 0
	raw, _ := json.Marshal(state)
	return sha256.Sum256(raw)
}

func erasedRecord(state *Snapshot, record memory.Record) bool {
	sum := sha256.Sum256([]byte(record.Content))
	digest := hex.EncodeToString(sum[:])
	return erasedAuthorship(state, record.Authorship) || state.ErasedPayloads["*:"+digest] || state.ErasedPayloads[record.Scope.Type+"/"+record.Scope.Value+":"+digest]
}
func eligibleRecord(record memory.Record) bool {
	if record.Sensitivity == "secret" {
		return false
	}
	now := time.Now()
	for _, bound := range []struct {
		value string
		start bool
	}{{record.ValidFrom, true}, {record.ValidUntil, false}} {
		if bound.value == "" {
			continue
		}
		stamp, err := time.Parse(time.RFC3339Nano, bound.value)
		if err != nil || (bound.start && now.Before(stamp)) || (!bound.start && !now.Before(stamp)) {
			return false
		}
	}
	return true
}

func (s *Store) Boundary(ctx context.Context, scopes []memory.Scope) (deadline string, err error) {
	err = s.transaction(ctx, false, func(state *Snapshot) error {
		now := time.Now()
		var earliest time.Time
		for _, entry := range state.Entries {
			if entry.Retired {
				continue
			}
			admitted := false
			for _, scope := range scopes {
				admitted = admitted || entry.Record.Scope == scope
			}
			if !admitted {
				continue
			}
			for _, value := range []string{entry.Record.ValidFrom, entry.Record.ValidUntil} {
				if value == "" {
					continue
				}
				stamp, e := time.Parse(time.RFC3339Nano, value)
				if e != nil {
					return memory.ErrUnavailable
				}
				if stamp.After(now) && (earliest.IsZero() || stamp.Before(earliest)) {
					earliest = stamp
				}
			}
		}
		if !earliest.IsZero() {
			deadline = earliest.UTC().Format(time.RFC3339Nano)
		}
		return nil
	})
	return
}
