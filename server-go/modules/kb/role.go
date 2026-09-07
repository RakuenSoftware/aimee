// Package kb composes the shared-knowledge role from standardized modules. It
// uses the same Vault, PostgreSQL and memory implementations as Server.
package kb

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/supervisor"
)

const PrincipalRef uint32 = 35
const EventIdentity uint32 = 4096 + PrincipalRef*256 + 1
const StageIdentity uint32 = 1

func NewHandler(home string) (bus.ModuleHandler, error) {
	instance, err := identity.Read(home)
	if err != nil {
		return nil, err
	}
	if instance.Role != identity.KB {
		return nil, errors.New("KB role does not match the first-boot identity")
	}
	return func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
		var request struct {
			Operation string `json:"operation"`
		}
		decoder := json.NewDecoder(bytes.NewReader(frame))
		decoder.DisallowUnknownFields()
		if invocation.StageID != StageIdentity || decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF || request.Operation != "identity" {
			return nil, bus.ModuleStatusInvalidRequest
		}
		reply, err := json.Marshal(instance)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return reply, bus.ModuleStatusOK
	}, nil
}

// Supervise composes the enabled standardized modules under this immutable role.
func Supervise(ctx context.Context, home, socket, manifest string) error {
	return supervisor.Run(ctx, home, "kb", socket, manifest)
}
