package main

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestCredentialVaultCapacityBindingAndCleanup(t *testing.T) {
	v := newCredentialVault(1)
	exp := time.Now().Add(time.Hour)
	if err := v.put("one", "iss", "sub", exp, "secret-one"); err != nil {
		t.Fatal(err)
	}
	if err := v.put("two", "iss", "sub", exp, "secret-two"); err == nil {
		t.Fatal("capacity overflow accepted")
	}
	s := &session{id: "one", iss: "iss", sub: "sub", oidcExpires: exp}
	if got, ok := v.get(s); !ok || got != "secret-one" {
		t.Fatalf("vault get = %q, %v", got, ok)
	}
	s.sub = "other"
	if _, ok := v.get(s); ok {
		t.Fatal("session identity mismatch accepted")
	}
	v.del("one")
	if _, ok := v.get(&session{id: "one", iss: "iss", sub: "sub", oidcExpires: exp}); ok {
		t.Fatal("deleted credential remained available")
	}
}

func TestConcurrentCredentialInsertionCompensatesLosingSessions(t *testing.T) {
	const contenders = 16
	srv := newTestServer(t, "http://127.0.0.1:1")
	srv.oidcTokens = newCredentialVault(1)
	srv.sessions.vault = srv.oidcTokens
	exp := time.Now().Add(time.Hour)
	p := &principal{iss: "iss", sub: "sub", expires: exp}
	sessions := make([]*session, contenders)
	for i := range sessions {
		var err error
		sessions[i], err = srv.sessions.create(p, false)
		if err != nil {
			t.Fatal(err)
		}
	}
	start := make(chan struct{})
	var wg sync.WaitGroup
	var successes atomic.Int32
	for _, sess := range sessions {
		wg.Add(1)
		go func(sess *session) {
			defer wg.Done()
			<-start
			if srv.retainOIDCCredential(sess, p, "token") == nil {
				successes.Add(1)
			}
		}(sess)
	}
	close(start)
	wg.Wait()
	if got := successes.Load(); got != 1 {
		t.Fatalf("successful insertions = %d, want 1", got)
	}
	var durable int
	if err := srv.sessions.db.QueryRow(`SELECT count(*) FROM sessions`).Scan(&durable); err != nil {
		t.Fatal(err)
	}
	if durable != 1 {
		t.Fatalf("durable sessions after capacity race = %d, want 1", durable)
	}
}

func TestFleetMutationClaimIsAtomic(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	sess, err := srv.sessions.create(&principal{iss: "iss", sub: "sub"}, false)
	if err != nil {
		t.Fatal(err)
	}
	const contenders = 16
	start := make(chan struct{})
	var wg sync.WaitGroup
	var claims atomic.Int32
	for i := 0; i < contenders; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			claimed, err := srv.sessions.claimFleetMutation(sess.id)
			if err != nil {
				t.Errorf("claim: %v", err)
				return
			}
			if claimed {
				claims.Add(1)
			}
		}()
	}
	close(start)
	wg.Wait()
	if got := claims.Load(); got != 1 {
		t.Fatalf("successful fleet mutation claims = %d, want 1", got)
	}
}

func TestRetainOIDCCredentialCompensatesSessionOnCapacityFailure(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	srv.oidcTokens = newCredentialVault(1)
	srv.sessions.vault = srv.oidcTokens
	exp := time.Now().Add(time.Hour)
	if err := srv.oidcTokens.put("occupied", "iss", "other", exp, "occupied-token"); err != nil {
		t.Fatal(err)
	}
	p := &principal{iss: "iss", sub: "sub", expires: exp}
	sess, err := srv.sessions.create(p, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.retainOIDCCredential(sess, p, "new-token"); err == nil {
		t.Fatal("capacity failure accepted")
	}
	if _, err := srv.sessions.get(sess.id); err == nil {
		t.Fatal("failed credential insertion left a durable session")
	}
}

func TestSessionExpiryCleansCredentialVault(t *testing.T) {
	srv := newTestServer(t, "http://127.0.0.1:1")
	exp := time.Now().Add(time.Hour)
	p := &principal{iss: "iss", sub: "sub", expires: exp}
	sess, err := srv.sessions.create(p, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := srv.oidcTokens.put(sess.id, sess.iss, sess.sub, exp, "token"); err != nil {
		t.Fatal(err)
	}
	if _, err := srv.sessions.db.Exec(`UPDATE sessions SET expires=? WHERE id=?`, time.Now().Add(-time.Second).Unix(), sess.id); err != nil {
		t.Fatal(err)
	}
	if _, err := srv.sessions.get(sess.id); err == nil {
		t.Fatal("expired session remained available")
	}
	if _, ok := srv.oidcTokens.get(sess); ok {
		t.Fatal("expired session left its OIDC credential in memory")
	}
}
