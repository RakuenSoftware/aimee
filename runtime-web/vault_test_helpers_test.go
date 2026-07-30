package main

import (
	"errors"
	"sync"
)

type fakeWebchatVault struct {
	mu       sync.Mutex
	snapshot webchatVaultSnapshot
	sealErr  error
}

func (v *fakeWebchatVault) Snapshot() (webchatVaultSnapshot, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	return v.snapshot, nil
}

func (v *fakeWebchatVault) Seal(record string, value []byte) error {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.sealErr != nil {
		return v.sealErr
	}
	s := string(append([]byte(nil), value...))
	switch record {
	case "accounts":
		v.snapshot.AccountsJSON = s
	case "session_hmac":
		v.snapshot.SessionHMAC = s
	case "tls_key":
		v.snapshot.TLSKey = s
	default:
		return errors.New("unexpected record")
	}
	return nil
}
