//go:build linux

// The PostgreSQL container's minimal storage supervisor. It has no Vault mount,
// credential provider or key generation path; core supplies the sole unlock key.
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"os/user"
	"strconv"
	"syscall"

	"github.com/JBailes/aimee/server-go/modules/postgres/storage"
	"golang.org/x/sys/unix"
)

func main() {
	if unix.Setrlimit(unix.RLIMIT_CORE, &unix.Rlimit{}) != nil || unix.Prctl(unix.PR_SET_DUMPABLE, 0, 0, 0, 0) != nil {
		os.Exit(1)
	}
	size := int64(32768)
	if raw := os.Getenv("AIMEE_POSTGRES_VOLUME_MIB"); raw != "" {
		n, err := strconv.ParseInt(raw, 10, 64)
		if err != nil || n < 256 || n > 16777216 {
			fmt.Fprintln(os.Stderr, "postgres storage: invalid volume size")
			os.Exit(1)
		}
		size = n
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	database, err := user.Lookup("postgres")
	if err != nil {
		fmt.Fprintln(os.Stderr, "postgres storage: database user unavailable")
		os.Exit(1)
	}
	uid, e1 := strconv.Atoi(database.Uid)
	gid, e2 := strconv.Atoi(database.Gid)
	if e1 != nil || e2 != nil {
		os.Exit(1)
	}
	volume := storage.Volume{Root: "/var/lib/aimee-postgres", Mount: "/var/lib/postgresql/data", Socket: "/run/aimee-postgres/storage.sock", Size: size << 20, Legacy: "/mnt/aimee-postgres-legacy", DatabaseUID: uid, DatabaseGID: gid}
	if err := volume.Run(ctx, []string{"/opt/aimee/postgres-secure-entrypoint.sh"}); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
