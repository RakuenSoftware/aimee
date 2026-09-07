package providers

import (
	"context"
	"errors"
	"golang.org/x/sys/unix"
	"os"
	"path/filepath"
	"time"
)

func modelServicesRootIsVolatile(root string) error {
	var stat unix.Statfs_t
	if unix.Statfs(root, &stat) != nil || stat.Type != unix.TMPFS_MAGIC {
		return errors.New("model service identities require a private tmpfs mount")
	}
	return nil
}

// Serialize the Vault read/create and materialization across competing first
// boots. The lock contains no credential and is scoped to the instance home.
func modelServicesLock(ctx context.Context, home string) (func(), error) {
	fd, err := unix.Open(filepath.Join(home, ".model-services.lock"), unix.O_CREAT|unix.O_RDWR|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return nil, err
	}
	var stat unix.Stat_t
	if err = unix.Fstat(fd, &stat); err != nil || stat.Mode&unix.S_IFMT != unix.S_IFREG || stat.Mode&0777 != 0600 || int(stat.Uid) != os.Geteuid() {
		unix.Close(fd)
		return nil, errors.New("invalid model identity bootstrap lock")
	}
	for {
		err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB)
		if err == nil {
			return func() { unix.Flock(fd, unix.LOCK_UN); unix.Close(fd) }, nil
		}
		if err != unix.EWOULDBLOCK && err != unix.EAGAIN {
			unix.Close(fd)
			return nil, err
		}
		select {
		case <-ctx.Done():
			unix.Close(fd)
			return nil, ctx.Err()
		case <-time.After(25 * time.Millisecond):
		}
	}
}
