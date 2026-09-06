//go:build !linux

package providers

import (
	"context"
	"errors"
)

type probeWorker struct{}

func newProbeWorker(context.Context, string) (*probeWorker, error) {
	return nil, errors.New("CLI diagnostics require Linux process isolation")
}
func (*probeWorker) probe(context.Context, string) (object, error) {
	return nil, errors.New("CLI diagnostics unavailable")
}
func RunProbeWorker([]string) (bool, int) { return false, 0 }
