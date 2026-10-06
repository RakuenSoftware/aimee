package egress

import (
	"bytes"
	"context"
	"errors"
	"strconv"
	"strings"
)

const maxResolvedCredentialBytes = 4096

type credentialResolver interface {
	Resolve(context.Context, uint32, string) ([]byte, error)
}

type vaultCredentialResolver struct {
	run func(context.Context, string) ([]byte, error)
}

func (r *vaultCredentialResolver) Resolve(ctx context.Context, principal uint32, handle string) ([]byte, error) {
	if r == nil || r.run == nil {
		return nil, errors.New("credential handle is unavailable")
	}
	var name string
	if principal == MemoryClientRef && handle == "memory-backend" {
		name = "AIMEE_MEMORY_BACKEND_TOKEN"
	} else {
		want := "mcp:" + strconv.FormatUint(uint64(principal), 10)
		if principal < 200+PluginClientOffset || principal >= 456+PluginClientOffset || handle != want {
			return nil, errors.New("credential handle is unavailable")
		}
		name = "AIMEE_MCP_" + strconv.FormatUint(uint64(principal), 10) + "_TOKEN"
	}
	secret, err := r.run(ctx, name)
	if err != nil || len(secret) == 0 || len(secret) > maxResolvedCredentialBytes || bytes.ContainsAny(secret, "\x00\r\n") {
		clear(secret)
		return nil, errors.New("credential handle is unavailable")
	}
	return secret, nil
}

func validMCPVaultName(name string) bool {
	if len(name) < len("AIMEE_MCP_X") || len(name) > 128 || !strings.HasPrefix(name, "AIMEE_MCP_") {
		return false
	}
	for _, char := range name {
		if !(char == '_' || char >= 'A' && char <= 'Z' || char >= '0' && char <= '9') {
			return false
		}
	}
	return strings.HasSuffix(name, "_TOKEN") || strings.HasSuffix(name, "_SECRET") ||
		strings.HasSuffix(name, "_API_KEY") || strings.HasSuffix(name, "_BEARER") ||
		strings.HasSuffix(name, "_CREDENTIAL")
}
