package main

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

type sessionStore interface {
	CreateSession(username string) (string, error)
	ValidateSession(token string) (string, error)
	DeleteSession(token string) error
	DeleteSessionsForUser(username string) error
}

// signedSessionStore persists only a random lookup ID. The browser credential
// is ID.HMAC(ID); the HMAC key is loaded from Vault into memory and never
// written to SQLite. A copied sessions row therefore cannot authenticate.
type signedSessionStore struct {
	db       *sql.DB
	key      []byte
	duration time.Duration
}

func newSignedSessionStore(db *sql.DB, vault webchatVaultStore, duration time.Duration) (*signedSessionStore, error) {
	snap, err := vault.Snapshot()
	if err != nil {
		return nil, err
	}
	key, err := hex.DecodeString(snap.SessionHMAC)
	if snap.SessionHMAC == "" {
		key = make([]byte, 32)
		if _, err = rand.Read(key); err != nil {
			return nil, err
		}
		encoded := []byte(hex.EncodeToString(key))
		if err = vault.Seal("session_hmac", encoded); err != nil {
			for i := range key {
				key[i] = 0
			}
			for i := range encoded {
				encoded[i] = 0
			}
			return nil, err
		}
		for i := range encoded {
			encoded[i] = 0
		}
	} else if err != nil || len(key) != 32 {
		return nil, errors.New("invalid Vault session HMAC key")
	}
	if err := migrateLegacySessions(db); err != nil {
		for i := range key {
			key[i] = 0
		}
		return nil, err
	}
	return &signedSessionStore{db: db, key: key, duration: duration}, nil
}

func migrateLegacySessions(db *sql.DB) error {
	if _, err := db.Exec(`CREATE TABLE IF NOT EXISTS webchat_security_migrations
		(version INTEGER PRIMARY KEY)`); err != nil {
		return err
	}
	res, err := db.Exec(`INSERT OR IGNORE INTO webchat_security_migrations(version) VALUES (1)`)
	if err != nil {
		return err
	}
	inserted, err := res.RowsAffected()
	if err != nil || inserted == 0 {
		return err
	}
	// Pre-migration rows contain bearer tokens. The new validator rejects their
	// shape, and removing them makes their invalidation explicit.
	_, err = db.Exec(`DELETE FROM sessions`)
	return err
}

func (s *signedSessionStore) sign(id string) string {
	mac := hmac.New(sha256.New, s.key)
	_, _ = mac.Write([]byte(id))
	return hex.EncodeToString(mac.Sum(nil))
}

func (s *signedSessionStore) parse(token string) (string, bool) {
	id, signature, ok := strings.Cut(token, ".")
	if !ok || len(id) != 48 || len(signature) != sha256.Size*2 {
		return "", false
	}
	if _, err := hex.DecodeString(id); err != nil {
		return "", false
	}
	expected, err := hex.DecodeString(s.sign(id))
	if err != nil {
		return "", false
	}
	actual, err := hex.DecodeString(signature)
	return id, err == nil && hmac.Equal(expected, actual)
}

func (s *signedSessionStore) CreateSession(username string) (string, error) {
	raw := make([]byte, 24)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}
	id := hex.EncodeToString(raw)
	for i := range raw {
		raw[i] = 0
	}
	now := time.Now().UTC()
	_, err := s.db.Exec(`INSERT INTO sessions(token,username,created_at,expires_at) VALUES(?,?,?,?)`,
		id, username, now.Format(time.RFC3339), now.Add(s.duration).Format(time.RFC3339))
	if err != nil {
		return "", fmt.Errorf("insert session: %w", err)
	}
	return id + "." + s.sign(id), nil
}

func (s *signedSessionStore) ValidateSession(token string) (string, error) {
	id, ok := s.parse(token)
	if !ok {
		return "", auth.ErrSessionNotFound
	}
	var username, expiresAt string
	err := s.db.QueryRow(`SELECT username,expires_at FROM sessions WHERE token=?`, id).Scan(&username, &expiresAt)
	if errors.Is(err, sql.ErrNoRows) {
		return "", auth.ErrSessionNotFound
	}
	if err != nil {
		return "", err
	}
	expires, err := time.Parse(time.RFC3339, expiresAt)
	if err != nil || time.Now().UTC().After(expires) {
		_, _ = s.db.Exec(`DELETE FROM sessions WHERE token=?`, id)
		return "", auth.ErrSessionNotFound
	}
	_, _ = s.db.Exec(`UPDATE sessions SET expires_at=? WHERE token=?`,
		time.Now().UTC().Add(s.duration).Format(time.RFC3339), id)
	return username, nil
}

func (s *signedSessionStore) DeleteSession(token string) error {
	id, ok := s.parse(token)
	if !ok {
		return nil
	}
	_, err := s.db.Exec(`DELETE FROM sessions WHERE token=?`, id)
	return err
}

func (s *signedSessionStore) DeleteSessionsForUser(username string) error {
	_, err := s.db.Exec(`DELETE FROM sessions WHERE username=?`, username)
	return err
}
