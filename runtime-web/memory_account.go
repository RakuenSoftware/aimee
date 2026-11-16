package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"syscall"
	"time"
)

var errMemoryAccountActivation = errors.New("private memory account activation unavailable")

func activateNativeMemoryAccount(username string) error {
	// A non-container/disabled-memory installation retains its existing account
	// behavior. A managed Native installation must complete its explicit grant.
	if _, err := os.Stat("/opt/aims-memory/server/etc/database.json"); errors.Is(err, os.ErrNotExist) {
		return nil
	} else if err != nil {
		return errMemoryAccountActivation
	}
	const helper = "/usr/local/bin/aimee-memory-activate-account"
	info, err := os.Lstat(helper)
	if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0o022 != 0 {
		return errMemoryAccountActivation
	}
	st, ok := info.Sys().(*syscall.Stat_t)
	if !ok || st.Uid != 0 {
		return errMemoryAccountActivation
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	// Passwords and native grants never enter argv; the root helper verifies PAM
	// group membership and atomically commits a single-owner activation roster.
	if err := exec.CommandContext(ctx, helper, username).Run(); err != nil {
		return errMemoryAccountActivation
	}
	return nil
}

func (p *pamAccounts) ActivateMemoryAccount(username string) error {
	if p.activateMemory != nil {
		return p.activateMemory(username)
	}
	return nil
}
