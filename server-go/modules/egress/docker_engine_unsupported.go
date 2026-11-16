//go:build !linux

package egress

import (
	"context"
	"errors"
)

func OpenDockerEngine(context.Context, string) (*DockerEngine, error) {
	return nil, errors.New("qualified Docker Unix control requires Linux")
}
