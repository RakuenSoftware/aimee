package postgres

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"time"
)

// storeDSN preserves explicit native deployment inputs. Container deployments
// read their two fixed credentials from the local core Vault resource instead
// of persisting database passwords in application container metadata. Neither
// DSNs nor the LUKS key traverse the event bus.
func storeDSN(ctx context.Context, name string) (string, error) {
	var operation byte
	switch name {
	case "AIMEE_STORE_URL":
		operation = 2
	case "AIMEE_STORE_MIGRATION_URL":
		operation = 3
	default:
		return "", errors.New("postgres: unsupported credential")
	}
	if value := os.Getenv(name); value != "" {
		return value, nil
	}
	home := os.Getenv("AIMEE_HOME")
	if home == "" {
		return "", errors.New("postgres: database credential unavailable")
	}
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "/usr/local/bin/aimee-server", "--postgres-vault-resource")
	cmd.Env = []string{"AIMEE_HOME=" + home, "PATH=/usr/local/bin:/usr/bin:/bin"}
	cmd.Stdin = bytes.NewReader([]byte{operation})
	reply, err := cmd.Output()
	defer clear(reply)
	if err != nil || len(reply) == 0 || len(reply) >= 4096 || bytes.IndexByte(reply, 0) >= 0 {
		return "", errors.New("postgres: database credential unavailable")
	}
	return string(reply), nil
}
