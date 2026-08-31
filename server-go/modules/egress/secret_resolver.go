package egress

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

const maxResolvedCredentialBytes = 4096

type credentialResolver interface {
	Resolve(context.Context, uint32, string) ([]byte, error)
}

type vaultCredentialResolver struct {
	home, helper string
	run          func(context.Context, string) ([]byte, error)
}

func newVaultCredentialResolver() credentialResolver {
	resolver := &vaultCredentialResolver{home: os.Getenv("AIMEE_HOME"),
		helper: os.Getenv("AIMEE_EGRESS_CREDENTIAL_HELPER")}
	resolver.run = resolver.runHelper
	return resolver
}

func (r *vaultCredentialResolver) Resolve(ctx context.Context, principal uint32, handle string) ([]byte, error) {
	want := "mcp:" + strconv.FormatUint(uint64(principal), 10)
	if r == nil || principal < 200+PluginClientOffset || principal >= 456+PluginClientOffset ||
		handle != want || r.run == nil {
		return nil, errors.New("credential handle is unavailable")
	}
	name := "AIMEE_MCP_" + strconv.FormatUint(uint64(principal), 10) + "_TOKEN"
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

func (r *vaultCredentialResolver) runHelper(ctx context.Context, name string) ([]byte, error) {
	if r.helper != "/usr/local/bin/aimee-server" && r.helper != "/usr/local/bin/aimee-kb" {
		return nil, errors.New("credential helper is unavailable")
	}
	timeout := 10 * time.Second
	if deadline, ok := ctx.Deadline(); ok && time.Until(deadline) < timeout {
		timeout = time.Until(deadline)
	}
	if timeout <= 0 {
		return nil, context.DeadlineExceeded
	}
	helperContext, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	command := exec.CommandContext(helperContext, r.helper, "--egress-vault-secret", name)
	command.Env = []string{"AIMEE_HOME=" + r.home, "PATH=/usr/local/bin:/usr/bin:/bin"}
	output, err := command.Output()
	if err != nil {
		clear(output)
		return nil, fmt.Errorf("credential helper failed")
	}
	return output, nil
}
