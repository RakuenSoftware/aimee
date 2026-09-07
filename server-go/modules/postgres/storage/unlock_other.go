//go:build !linux

package storage

import (
	"context"
	"fmt"
	"os"
)

func Bootstrap(args []string) (bool, int) {
	if len(args) > 1 && args[1] == "__aimee_postgres_unlock" {
		fmt.Fprintln(os.Stderr, "postgres storage: LUKS requires a Linux container host")
		return true, 1
	}
	return false, 0
}
func MaintainUnlock(context.Context, string, string) {}
