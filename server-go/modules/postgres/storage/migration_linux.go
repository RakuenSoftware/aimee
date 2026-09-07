//go:build linux

package storage

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// clusterDigest rejects external tablespaces/WAL links and special files. The
// caller must stop the old postmaster; mounting its volume read-only alone does
// not exclude another container still writing to that volume.
func clusterDigest(ctx context.Context, root string) (string, error) {
	hash := sha256.New()
	err := filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		relative, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.IsDir() && !info.Mode().IsRegular() {
			return errors.New("postgres storage: legacy cluster has unsupported links or special files")
		}
		if entry.Name() == "postmaster.pid" {
			return errors.New("postgres storage: stop the legacy PostgreSQL instance before migration")
		}
		size := info.Size()
		if info.IsDir() {
			size = 0
		}
		fmt.Fprintf(hash, "%s\x00%d\x00%d\x00", relative, info.Mode(), size)
		if info.Mode().IsRegular() {
			f, err := os.Open(path)
			if err != nil {
				return err
			}
			_, err = io.Copy(hash, f)
			closeErr := f.Close()
			if err != nil {
				return err
			}
			return closeErr
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

// migrateLegacy copies an offline cluster entirely inside the unlocked
// filesystem, verifies it against an unchanged source, then atomically adopts
// it. The original remains available for validated rollback. No PostgreSQL
// process can start until this function commits its durable migration marker.
func (v *Volume) migrateLegacy(ctx context.Context) error {
	if v.Legacy == "" || v.state.Migrated {
		return nil
	}
	if !filepath.IsAbs(v.Legacy) {
		return errors.New("postgres storage: absolute legacy storage path required")
	}
	source := filepath.Join(v.Legacy, "pgdata")
	version, err := os.ReadFile(filepath.Join(source, "PG_VERSION"))
	if errors.Is(err, os.ErrNotExist) {
		entries, readErr := os.ReadDir(v.Legacy)
		if errors.Is(readErr, os.ErrNotExist) || (readErr == nil && len(entries) == 0) {
			return nil
		}
		return errors.New("postgres storage: unrecognized legacy data; migration required")
	}
	if err != nil {
		return err
	}
	if strings.TrimSpace(string(version)) != "18" {
		return errors.New("postgres storage: legacy cluster requires a PostgreSQL major-version upgrade")
	}
	before, err := clusterDigest(ctx, source)
	if err != nil {
		return err
	}
	target := filepath.Join(v.Mount, "pgdata")
	if _, err := os.Lstat(target); err == nil {
		// Recovery after atomic adoption but before the migration marker was synced.
		adopted, err := clusterDigest(ctx, target)
		if err != nil || adopted != before {
			return errors.New("postgres storage: existing database differs from the legacy cluster")
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	} else {
		staging := filepath.Join(v.Mount, ".legacy-migration")
		// Staging is never a live database and never contains the only copy. An
		// interrupted copy can be discarded and rebuilt from the unchanged source.
		if err := os.RemoveAll(staging); err != nil {
			return err
		}
		if err := os.Mkdir(staging, 0700); err != nil {
			return err
		}
		err = filepath.WalkDir(source, func(path string, entry fs.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			if err := ctx.Err(); err != nil {
				return err
			}
			relative, err := filepath.Rel(source, path)
			if err != nil {
				return err
			}
			destination := filepath.Join(staging, relative)
			info, err := entry.Info()
			if err != nil {
				return err
			}
			if !info.IsDir() && !info.Mode().IsRegular() {
				return errors.New("postgres storage: legacy cluster changed during migration")
			}
			if info.IsDir() {
				if err := os.MkdirAll(destination, info.Mode().Perm()); err != nil {
					return err
				}
			} else {
				input, err := os.Open(path)
				if err != nil {
					return err
				}
				output, err := os.OpenFile(destination, os.O_CREATE|os.O_EXCL|os.O_WRONLY, info.Mode().Perm())
				if err != nil {
					input.Close()
					return err
				}
				_, copyErr := io.Copy(output, input)
				input.Close()
				syncErr := output.Sync()
				closeErr := output.Close()
				if copyErr != nil {
					return copyErr
				}
				if syncErr != nil {
					return syncErr
				}
				if closeErr != nil {
					return closeErr
				}
			}
			if err := os.Chmod(destination, info.Mode().Perm()); err != nil {
				return err
			}
			return os.Chown(destination, v.DatabaseUID, v.DatabaseGID)
		})
		if err != nil {
			return err
		}
		copied, err := clusterDigest(ctx, staging)
		if err != nil {
			return err
		}
		after, err := clusterDigest(ctx, source)
		if err != nil {
			return err
		}
		if before != after || copied != before {
			return errors.New("postgres storage: legacy migration verification failed")
		}
		// Sync directories after all entries have been created, before publishing.
		if err := filepath.WalkDir(staging, func(path string, e fs.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if e.IsDir() {
				return syncDirectory(path)
			}
			return nil
		}); err != nil {
			return err
		}
		if err := os.Rename(staging, target); err != nil {
			return err
		}
		if err := syncDirectory(v.Mount); err != nil {
			return err
		}
	}
	v.state.Migrated = true
	return v.save()
}
