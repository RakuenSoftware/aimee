package main

import (
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"errors"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

const (
	sessionIdleTimeout     = 30 * time.Minute
	sessionAbsoluteTimeout = 8 * time.Hour
	breakGlassSessionTTL   = 300 * time.Second
)

// session is a logged-in console session, bound to a verified (iss, sub) so a
// lifted DB row is not portable across identities/IdPs.
type session struct {
	id                 string
	csrf               string
	iss                string
	sub                string
	breakGlass         bool
	created            time.Time
	lastSeen           time.Time
	expires            time.Time
	oidcExpires        time.Time
	fleetIndeterminate bool
}

type sessionStore struct {
	db    *sql.DB
	vault *credentialVault
}

// openSessionStore opens (creating if needed) the SQLite session DB. The caller
// is responsible for the 0600 file mode on the DB path.
func openSessionStore(path string) (*sessionStore, error) {
	db, err := sql.Open("sqlite3", path+"?_busy_timeout=5000&_journal_mode=WAL")
	if err != nil {
		return nil, err
	}
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS sessions (
		id TEXT PRIMARY KEY,
		csrf TEXT NOT NULL,
		iss TEXT NOT NULL,
		sub TEXT NOT NULL,
		break_glass INTEGER NOT NULL DEFAULT 0,
		created INTEGER NOT NULL,
		last_seen INTEGER NOT NULL,
		expires INTEGER NOT NULL,
		oidc_expires INTEGER NOT NULL DEFAULT 0,
		fleet_indeterminate INTEGER NOT NULL DEFAULT 0
	)`); err != nil {
		return nil, err
	}
	if _, err := db.Exec(`ALTER TABLE sessions ADD COLUMN oidc_expires INTEGER NOT NULL DEFAULT 0`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		_ = db.Close()
		return nil, err
	}
	if _, err := db.Exec(`ALTER TABLE sessions ADD COLUMN fleet_indeterminate INTEGER NOT NULL DEFAULT 0`); err != nil &&
		!strings.Contains(err.Error(), "duplicate column name") {
		_ = db.Close()
		return nil, err
	}
	return &sessionStore{db: db}, nil
}

func randToken() string {
	b := make([]byte, 32)
	_, _ = rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}

// create makes a fresh session bound to (iss, sub). A break-glass session gets a
// short absolute TTL. The id is always fresh — login rotates by creating anew.
func (s *sessionStore) create(p *principal, breakGlass bool) (*session, error) {
	now := time.Now()
	abs := sessionAbsoluteTimeout
	if breakGlass {
		abs = breakGlassSessionTTL
	}
	expires := now.Add(abs)
	if !breakGlass && !p.expires.IsZero() && p.expires.Before(expires) {
		expires = p.expires
	}
	sess := &session{
		id: randToken(), csrf: randToken(),
		iss: p.iss, sub: p.sub, breakGlass: breakGlass,
		created: now, lastSeen: now, expires: expires, oidcExpires: p.expires,
	}
	bg := 0
	if breakGlass {
		bg = 1
	}
	oidcExpires := int64(0)
	if !sess.oidcExpires.IsZero() {
		oidcExpires = sess.oidcExpires.Unix()
	}
	_, err := s.db.Exec(
		`INSERT INTO sessions(id,csrf,iss,sub,break_glass,created,last_seen,expires,oidc_expires,fleet_indeterminate) VALUES(?,?,?,?,?,?,?,?,?,0)`,
		sess.id, sess.csrf, sess.iss, sess.sub, bg, now.Unix(), now.Unix(), sess.expires.Unix(),
		oidcExpires)
	if err != nil {
		return nil, err
	}
	return sess, nil
}

// get validates a session id (absolute expiry + idle timeout) and refreshes
// last_seen. A stale session is deleted and treated as absent.
func (s *sessionStore) get(id string) (*session, error) {
	if id == "" {
		return nil, errors.New("no session")
	}
	row := s.db.QueryRow(
		`SELECT id,csrf,iss,sub,break_glass,created,last_seen,expires,oidc_expires,fleet_indeterminate FROM sessions WHERE id=?`, id)
	var sess session
	var bg, created, lastSeen, expires, oidcExpires, fleetIndeterminate int64
	if err := row.Scan(&sess.id, &sess.csrf, &sess.iss, &sess.sub, &bg, &created, &lastSeen,
		&expires, &oidcExpires, &fleetIndeterminate); err != nil {
		return nil, errors.New("no session")
	}
	now := time.Now()
	if now.Unix() >= expires || now.Unix()-lastSeen > int64(sessionIdleTimeout.Seconds()) {
		_, _ = s.db.Exec(`DELETE FROM sessions WHERE id=?`, id)
		if s.vault != nil {
			s.vault.del(id)
		}
		return nil, errors.New("session expired")
	}
	sess.breakGlass = bg == 1
	sess.fleetIndeterminate = fleetIndeterminate == 1
	sess.created = time.Unix(created, 0)
	sess.lastSeen = time.Unix(lastSeen, 0)
	sess.expires = time.Unix(expires, 0)
	if oidcExpires > 0 {
		sess.oidcExpires = time.Unix(oidcExpires, 0)
	}
	_, _ = s.db.Exec(`UPDATE sessions SET last_seen=? WHERE id=?`, now.Unix(), id)
	return &sess, nil
}

func (s *sessionStore) claimFleetMutation(id string) (bool, error) {
	result, err := s.db.Exec(`UPDATE sessions SET fleet_indeterminate=1 WHERE id=? AND fleet_indeterminate=0`, id)
	if err != nil {
		return false, err
	}
	n, err := result.RowsAffected()
	if err != nil {
		return false, err
	}
	return n == 1, nil
}

func (s *sessionStore) clearFleetMutation(id string) error {
	result, err := s.db.Exec(`UPDATE sessions SET fleet_indeterminate=0 WHERE id=?`, id)
	if err != nil {
		return err
	}
	n, err := result.RowsAffected()
	if err != nil || n != 1 {
		return errors.New("session unavailable")
	}
	return nil
}

func (s *sessionStore) del(id string) {
	_, _ = s.db.Exec(`DELETE FROM sessions WHERE id=?`, id)
	if s.vault != nil {
		s.vault.del(id)
	}
}

// invalidateSub drops all sessions for a principal. S0 provides the mechanism;
// S2a wires the enrollment-revoke path to call it.
func (s *sessionStore) invalidateSub(iss, sub string) {
	_, _ = s.db.Exec(`DELETE FROM sessions WHERE iss=? AND sub=?`, iss, sub)
	if s.vault != nil {
		s.vault.delPrincipal(iss, sub)
	}
}

func (s *sessionStore) close() error {
	if s.vault != nil {
		s.vault.clear()
	}
	return s.db.Close()
}
