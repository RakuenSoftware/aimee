//go:build linux

package storage

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"

	"golang.org/x/sys/unix"
)

// RunPlain starts PostgreSQL on an ordinary persistent directory. It shares the
// offline migration and TLS/role initialization with LUKS, but never opens a
// device, obtains a Vault encryption key, or mounts a filesystem.
func (v *Volume) RunPlain(ctx context.Context, command []string) error {
	if !filepath.IsAbs(v.Root) || len(command) == 0 {
		return errors.New("postgres storage: absolute storage path and database command required")
	}
	if err := os.MkdirAll(v.Root, 0755); err != nil {
		return err
	}
	info, err := os.Lstat(v.Root)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("postgres storage: invalid storage directory")
	}
	fd, err := unix.Open(filepath.Join(v.Root, "volume.lock"), unix.O_CREAT|unix.O_RDWR|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return err
	}
	defer unix.Close(fd)
	if err := unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err != nil {
		return errors.New("postgres storage: volume already owned")
	}
	entries, err := os.ReadDir(v.Root)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.Name() != "volume.lock" && entry.Name() != "plain" {
			return errors.New("postgres storage: existing encrypted or unrecognized data; select LUKS for an encrypted store or migrate explicitly")
		}
	}
	v.Root = filepath.Join(v.Root, "plain")
	v.Mount = v.Root
	if err := os.MkdirAll(v.Root, 0755); err != nil {
		return err
	}
	info, err = os.Lstat(v.Root)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("postgres storage: invalid plain storage directory")
	}
	path := filepath.Join(v.Root, "volume.json")
	if err := regularPrivate(path); errors.Is(err, os.ErrNotExist) {
		entries, err := os.ReadDir(v.Root)
		if err != nil || len(entries) != 0 {
			return errors.New("postgres storage: unrecognized plain storage directory")
		}
		v.state = manifest{Version: 1, Phase: "plain"}
		if err := v.save(); err != nil {
			return err
		}
	} else if err != nil {
		return err
	} else {
		data, err := os.ReadFile(path)
		if err != nil || len(data) > 1024 {
			return errors.New("postgres storage: invalid plain storage manifest")
		}
		decoder := json.NewDecoder(bytes.NewReader(data))
		decoder.DisallowUnknownFields()
		if decoder.Decode(&v.state) != nil || decoder.Decode(new(any)) != io.EOF || v.state.Version != 1 || v.state.Phase != "plain" || v.state.UUID != "" || v.state.Bytes != 0 {
			return errors.New("postgres storage: invalid plain storage manifest")
		}
	}
	if err := v.migrateLegacy(ctx); err != nil {
		return err
	}
	return runDatabase(ctx, command, nil)
}
