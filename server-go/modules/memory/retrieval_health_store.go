package memory

import (
	"context"
	"encoding/json"
	"errors"
	"sync"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
)

// Optional telemetry stays in the serving owner's existing PostgreSQL
// capability. It does not add a database or change required WORM retention.
const healthStoreSchema = `CREATE TABLE IF NOT EXISTS memory_retrieval_health_namespaces (
 principal text NOT NULL CHECK (octet_length(principal) BETWEEN 1 AND 128),
 project text NOT NULL CHECK (octet_length(project)<=1024),
 workspace text NOT NULL CHECK (octet_length(workspace)<=1024),
 state text NOT NULL CHECK (octet_length(state) BETWEEN 1 AND 393216),
 updated_at timestamptz NOT NULL DEFAULT now(),
 PRIMARY KEY(principal,project,workspace)
)`

type healthOwnerState struct {
	mu    sync.Mutex
	ready bool
}

func (s *postgresDataStore) ensureHealthStore(ctx context.Context) error {
	if s.placement != PlacementServer || s.health == nil {
		return errors.New("health serving owner unavailable")
	}
	s.health.mu.Lock()
	defer s.health.mu.Unlock()
	if s.health.ready {
		return nil
	}
	db, ok := s.db.(store.Store)
	if !ok {
		return errors.New("health migration capability unavailable")
	}
	statements := []string{healthStoreSchema}
	if err := db.Migrate(ctx, store.MigrationRequest{Owner: "memory-health", Version: 1, Statements: statements, Checksum: store.StoreChecksum(statements)}); err != nil {
		return err
	}
	s.health.ready = true
	return nil
}

// principal must come from authenticated command context, not request fields.
// The transaction serializes initial scope creation as well as updates, so
// concurrent requests cannot exceed the namespace quota or lose a late stage.
func (s *postgresDataStore) updateHealthStore(ctx context.Context, principal, project, workspace string, now time.Time, update func(*healthJournal) error) (*healthJournal, error) {
	if principal == "" || len(principal) > 128 || len(project) > 1024 || len(workspace) > 1024 || update == nil {
		return nil, errors.New("invalid health owner context")
	}
	if err := s.ensureHealthStore(ctx); err != nil {
		return nil, err
	}
	db, ok := s.db.(store.DB)
	if !ok {
		return nil, errors.New("health transaction capability unavailable")
	}
	tx, err := db.Begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback(context.WithoutCancel(ctx))
	journal, err := updateHealthTransaction(ctx, tx, principal, project, workspace, now, update)
	if err != nil {
		return nil, err
	}
	if err = tx.Commit(ctx); err != nil {
		return nil, err
	}
	return journal, nil
}

func updateHealthTransaction(ctx context.Context, tx store.Tx, principal, project, workspace string, now time.Time, update func(*healthJournal) error) (*healthJournal, error) {
	if _, err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock(74208,hashtext($1))`, principal); err != nil {
		return nil, err
	}
	var namespace string
	if err := tx.QueryRow(ctx, `SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1`).Scan(&namespace); err != nil {
		return nil, err
	}
	var raw string
	err := tx.QueryRow(ctx, `SELECT state FROM memory_retrieval_health_namespaces WHERE principal=$1 AND project=$2 AND workspace=$3 FOR UPDATE`, principal, project, workspace).Scan(&raw)
	var journal *healthJournal
	if store.IsNoRows(err) {
		var count int
		if err = tx.QueryRow(ctx, `SELECT count(*) FROM memory_retrieval_health_namespaces WHERE principal=$1`, principal).Scan(&count); err != nil {
			return nil, err
		}
		if count >= 128 {
			return nil, errors.New("health namespace capacity exhausted")
		}
		journal, err = newHealthJournal(namespace, principal, project, workspace, now)
	} else if err == nil {
		journal, err = decodeHealthJournal([]byte(raw))
	}
	if err != nil {
		return nil, err
	}
	if journal.Namespace != namespace || journal.Principal != principal || journal.Project != project || journal.Workspace != workspace {
		return nil, errors.New("persisted health owner mismatch")
	}
	if err = update(journal); err != nil {
		return nil, err
	}
	next, err := json.Marshal(journal)
	if err != nil || len(next) > healthMaxStateBytes {
		return nil, errors.New("health state exceeds persistence bound")
	}
	if _, err = tx.Exec(ctx, `INSERT INTO memory_retrieval_health_namespaces(principal,project,workspace,state)
 VALUES($1,$2,$3,$4) ON CONFLICT(principal,project,workspace)
 DO UPDATE SET state=EXCLUDED.state,updated_at=now()`, principal, project, workspace, string(next)); err != nil {
		return nil, err
	}
	return journal, nil
}

func (s *postgresDataStore) readHealthStore(ctx context.Context, principal, project, workspace string) (*healthJournal, error) {
	if principal == "" || len(principal) > 128 || len(project) > 1024 || len(workspace) > 1024 {
		return nil, errors.New("invalid health owner context")
	}
	if err := s.ensureHealthStore(ctx); err != nil {
		return nil, err
	}
	var namespace, raw string
	err := s.db.QueryRow(ctx, `SELECT g.owner_id::text,h.state FROM memory_retrieval_health_namespaces h
 CROSS JOIN user_memory_collection_generation g WHERE g.id=1 AND h.principal=$1 AND h.project=$2 AND h.workspace=$3`, principal, project, workspace).Scan(&namespace, &raw)
	if err != nil {
		return nil, err
	}
	journal, err := decodeHealthJournal([]byte(raw))
	if err != nil {
		return nil, err
	}
	if journal.Namespace != namespace || journal.Principal != principal || journal.Project != project || journal.Workspace != workspace {
		return nil, errors.New("persisted health owner mismatch")
	}
	return journal, nil
}
