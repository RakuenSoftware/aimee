//go:build !linux

package main

import "errors"

func installModuleNetworkGuard() error {
	return errors.New("module network isolation is unavailable on this platform")
}

func hardenEgressCredentialOwner() error { return nil }
