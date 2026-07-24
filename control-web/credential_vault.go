package main

import (
	"errors"
	"sync"
	"time"
)

const maxOIDCCredentials = 4096

type oidcCredential struct {
	token   []byte
	iss     string
	sub     string
	expires time.Time
}

// credentialVault deliberately keeps forwarded OIDC bearer material in memory
// only. A console restart therefore requires OIDC users to authenticate again.
type credentialVault struct {
	mu      sync.Mutex
	max     int
	entries map[string]*oidcCredential
}

func newCredentialVault(max int) *credentialVault {
	if max <= 0 {
		max = maxOIDCCredentials
	}
	return &credentialVault{max: max, entries: make(map[string]*oidcCredential)}
}

func cleanseBytes(b []byte) {
	for i := range b {
		b[i] = 0
	}
}

func (v *credentialVault) purgeExpiredLocked(now time.Time) {
	for id, e := range v.entries {
		if !now.Before(e.expires) {
			cleanseBytes(e.token)
			delete(v.entries, id)
		}
	}
}

func (v *credentialVault) put(id, iss, sub string, expires time.Time, raw string) error {
	if v == nil || id == "" || iss == "" || sub == "" || raw == "" || !time.Now().Before(expires) {
		return errors.New("invalid OIDC credential")
	}
	copyToken := []byte(raw)
	v.mu.Lock()
	defer v.mu.Unlock()
	v.purgeExpiredLocked(time.Now())
	if _, exists := v.entries[id]; exists || len(v.entries) >= v.max {
		cleanseBytes(copyToken)
		return errors.New("OIDC credential capacity reached")
	}
	v.entries[id] = &oidcCredential{token: copyToken, iss: iss, sub: sub, expires: expires}
	return nil
}

func (v *credentialVault) get(sess *session) (string, bool) {
	if v == nil || sess == nil || sess.id == "" || sess.oidcExpires.IsZero() {
		return "", false
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	v.purgeExpiredLocked(time.Now())
	e, ok := v.entries[sess.id]
	if !ok || e.iss != sess.iss || e.sub != sess.sub ||
		e.expires.Unix() != sess.oidcExpires.Unix() {
		return "", false
	}
	return string(e.token), true
}

func (v *credentialVault) del(id string) {
	if v == nil || id == "" {
		return
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	if e := v.entries[id]; e != nil {
		cleanseBytes(e.token)
		delete(v.entries, id)
	}
}

func (v *credentialVault) delPrincipal(iss, sub string) {
	if v == nil {
		return
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	for id, e := range v.entries {
		if e.iss == iss && e.sub == sub {
			cleanseBytes(e.token)
			delete(v.entries, id)
		}
	}
}

func (v *credentialVault) clear() {
	if v == nil {
		return
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	for id, e := range v.entries {
		cleanseBytes(e.token)
		delete(v.entries, id)
	}
}
