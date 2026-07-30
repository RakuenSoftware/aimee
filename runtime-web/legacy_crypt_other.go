//go:build !linux || !cgo

package main

// Legacy /etc/shadow verifier migration is Linux-only. Other platforms never
// created the container PAM registry and therefore have nothing to migrate.
func verifyLegacyPassword(string, string) bool { return false }
