// Package kb composes the shared-knowledge role from standardized modules. It
// uses the same Vault, PostgreSQL and memory implementations as Server.
package kb

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"

	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
	"github.com/JBailes/aimee/server-go/modules/module-runtime/supervisor"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const PrincipalRef uint32 = 35
const EventIdentity uint32 = 4096 + PrincipalRef*256 + 1
const StageIdentity uint32 = 1

func NewHandler(home string) (ring.ModuleHandler, error) {
	instance, err := identity.Read(home)
	if err != nil {
		return nil, err
	}
	if instance.Role != identity.KB {
		return nil, errors.New("KB role does not match the first-boot identity")
	}
	return func(invocation ring.ModuleInvocation, frame []byte) ([]byte, ring.ModuleStatus) {
		var request struct {
			Operation string `json:"operation"`
		}
		decoder := json.NewDecoder(bytes.NewReader(frame))
		decoder.DisallowUnknownFields()
		if invocation.StageID != StageIdentity || decoder.Decode(&request) != nil || decoder.Decode(new(any)) != io.EOF || request.Operation != "identity" {
			return nil, ring.ModuleStatusInvalidRequest
		}
		reply, err := json.Marshal(instance)
		if err != nil {
			return nil, ring.ModuleStatusInternal
		}
		return reply, ring.ModuleStatusOK
	}, nil
}

// Supervise composes the enabled standardized modules under this immutable role.
func Supervise(ctx context.Context, home, socket, manifest string) error {
	return supervisor.Run(ctx, home, "kb", socket, manifest)
}
