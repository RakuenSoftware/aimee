//go:build !linux

package delegates

import (
	"context"
	"errors"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// PrivateWorkerLease is unavailable without Linux's held resource custody.
type PrivateWorkerLease struct{}

func AcquirePrivateWorkerLease(string) (*PrivateWorkerLease, error) {
	return nil, errors.New("private verifier resource ownership requires Linux")
}
func (*PrivateWorkerLease) Labels() map[string]string { return nil }
func (*PrivateWorkerLease) Close(bool)                {}
func ReapPrivateWorkers(context.Context, string, *egress.DockerEngine) (int, error) {
	return 0, errors.New("private verifier orphan recovery requires Linux")
}
