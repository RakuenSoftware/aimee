//go:build linux

package storage

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

type manifest struct {
	Version  int    `json:"version"`
	UUID     string `json:"uuid"`
	Phase    string `json:"phase"`
	Bytes    int64  `json:"bytes"`
	Migrated bool   `json:"legacy_migrated,omitempty"`
}

// Volume supervises a single encrypted PostgreSQL filesystem. Its persistent
// directory contains only a public manifest, a lock and the LUKS ciphertext.
type Volume struct {
	Root, Mount, Socket      string
	Legacy                   string
	DatabaseUID, DatabaseGID int
	Size                     int64
	owner                    *os.File
	loop                     *os.File
	state                    manifest
	mapped, mounted          bool
	mu                       sync.Mutex
	ready                    bool
	unlocked                 bool
}

func (v *Volume) image() string   { return filepath.Join(v.Root, "postgres.luks") }
func (v *Volume) mapping() string { return "aimee-pg-" + v.state.UUID }
func (v *Volume) device() string  { return "/dev/mapper/" + v.mapping() }

func publicCommand(ctx context.Context, name string, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Env = []string{"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C"}
	data, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("postgres storage: %s failed", name)
	}
	return data, nil
}
func keyCommand(ctx context.Context, key *secret, args ...string) error {
	cmd := exec.CommandContext(ctx, "cryptsetup", args...)
	cmd.Env = []string{"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C"}
	cmd.Stdin = bytes.NewReader(key.bytes())
	cmd.Stderr = os.Stderr
	// No debug or dump-volume-key options are accepted; ordinary cryptsetup
	// diagnostics describe the failed operation and never print key material.
	if cmd.Run() != nil {
		return fmt.Errorf("postgres storage: cryptsetup %s refused the volume", args[0])
	}
	return nil
}
func syncDirectory(path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Sync()
}
func (v *Volume) save() error {
	data, err := json.Marshal(v.state)
	if err != nil {
		return err
	}
	f, err := os.CreateTemp(v.Root, ".manifest-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(data); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if err = os.Rename(f.Name(), filepath.Join(v.Root, "volume.json")); err != nil {
		return err
	}
	return syncDirectory(v.Root)
}
func regularPrivate(path string) error {
	st, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !st.Mode().IsRegular() || st.Mode().Perm() != 0600 {
		return errors.New("postgres storage: invalid persistent file")
	}
	native, ok := st.Sys().(*syscall.Stat_t)
	if !ok || native.Uid != uint32(os.Geteuid()) || native.Nlink != 1 {
		return errors.New("postgres storage: invalid file ownership")
	}
	return nil
}
func (v *Volume) Prepare() error {
	if !filepath.IsAbs(v.Root) || !filepath.IsAbs(v.Mount) || !filepath.IsAbs(v.Socket) || v.Root == v.Mount {
		return errors.New("postgres storage: root and distinct absolute storage paths required")
	}
	if v.Size < 256<<20 || v.Size > 16<<40 {
		return errors.New("postgres storage: volume size out of bounds")
	}
	if err := os.MkdirAll(v.Root, 0700); err != nil {
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
	v.owner = os.NewFile(uintptr(fd), "volume.lock")
	if err := unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err != nil {
		v.owner.Close()
		v.owner = nil
		return errors.New("postgres storage: volume already owned")
	}
	path := filepath.Join(v.Root, "volume.json")
	if err := regularPrivate(path); errors.Is(err, os.ErrNotExist) {
		entries, err := os.ReadDir(v.Root)
		if err != nil {
			return err
		}
		for _, e := range entries {
			if e.Name() != "volume.lock" {
				return errors.New("postgres storage: refusing unrecognized existing data")
			}
		}
		var id [16]byte
		if _, err := rand.Read(id[:]); err != nil {
			return err
		}
		id[6] = (id[6] & 15) | 64
		id[8] = (id[8] & 63) | 128
		uuid := fmt.Sprintf("%x-%x-%x-%x-%x", id[:4], id[4:6], id[6:8], id[8:10], id[10:])
		v.state = manifest{Version: 1, UUID: uuid, Phase: "allocated", Bytes: v.Size}
		return v.save()
	} else if err != nil {
		return err
	}
	data, err := os.ReadFile(path)
	if err != nil || len(data) > 1024 {
		return errors.New("postgres storage: invalid volume manifest")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&v.state) != nil || decoder.Decode(new(any)) != io.EOF || v.state.Version != 1 || !validVolumeID(v.state.UUID) ||
		(v.state.Phase != "allocated" && v.state.Phase != "filesystem") || v.state.Bytes < 256<<20 || v.state.Bytes > 16<<40 {
		return errors.New("postgres storage: invalid volume manifest")
	}
	if v.Size != v.state.Bytes {
		return errors.New("postgres storage: resize requires an explicit storage migration")
	}
	if err := regularPrivate(v.image()); err != nil && !(errors.Is(err, os.ErrNotExist) && v.state.Phase == "allocated") {
		return err
	}
	return nil
}
func (v *Volume) needsInitialize() bool {
	_, err := os.Lstat(v.image())
	return errors.Is(err, os.ErrNotExist) && v.state.Phase == "allocated"
}
func (v *Volume) unlock(ctx context.Context, key *secret) error {
	if v.needsInitialize() {
		f, err := os.OpenFile(v.image(), os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
		if err != nil {
			return err
		}
		err = f.Truncate(v.state.Bytes)
		if err == nil {
			err = f.Sync()
		}
		closeErr := f.Close()
		if err != nil {
			return err
		}
		if closeErr != nil {
			return closeErr
		}
		if err = syncDirectory(v.Root); err != nil {
			return err
		}
		if err := keyCommand(ctx, key, "luksFormat", "--batch-mode", "--type", "luks2", "--uuid", v.state.UUID,
			"--cipher", "aes-xts-plain64", "--key-size", "512", "--pbkdf", "argon2id", "--pbkdf-memory", "65536",
			"--pbkdf-parallel", "1", "--iter-time", "2000", "--key-file", "-", "--keyfile-size", "32", v.image()); err != nil {
			return err
		}
	}
	if err := regularPrivate(v.image()); err != nil {
		return err
	}
	id, err := publicCommand(ctx, "cryptsetup", "luksUUID", v.image())
	if err != nil || strings.TrimSpace(string(id)) != v.state.UUID {
		return errors.New("postgres storage: missing or mismatched LUKS header")
	}
	// A stopped/crashed container may leave an unmounted device-mapper target.
	// The exclusive volume lock and matching cryptsetup UUID bind cleanup to us;
	// cryptsetup refuses to remove a mapping still mounted by a live container.
	if uuid, err := publicCommand(ctx, "dmsetup", "info", "--columns", "--noheadings", "-o", "uuid", v.mapping()); err == nil {
		if !strings.HasPrefix(strings.TrimSpace(string(uuid)), "CRYPT-LUKS2-"+strings.ReplaceAll(v.state.UUID, "-", "")+"-") {
			return errors.New("postgres storage: conflicting device mapping")
		}
		if _, err = publicCommand(ctx, "cryptsetup", "close", v.mapping()); err != nil {
			return err
		}
	}
	if err := v.attachLoop(); err != nil {
		return err
	}
	if err := keyCommand(ctx, key, "open", "--type", "luks2", "--disable-keyring", "--key-file", "-", "--keyfile-size", "32", v.loop.Name(), v.mapping()); err != nil {
		return err
	}
	v.mapped = true
	fs, fsErr := publicCommand(ctx, "blkid", "-p", "-s", "TYPE", "-o", "value", v.device())
	if fsErr != nil {
		if v.state.Phase != "allocated" {
			return errors.New("postgres storage: existing filesystem unavailable")
		}
		// Only the just-initialized LUKS volume may receive a filesystem. A probe
		// error on a previously committed filesystem never causes reformatting.
		if _, err := publicCommand(ctx, "mkfs.ext4", "-q", "-m", "0", v.device()); err != nil {
			return err
		}
	} else if strings.TrimSpace(string(fs)) != "ext4" {
		return errors.New("postgres storage: unsupported existing filesystem")
	}
	if err := os.MkdirAll(v.Mount, 0700); err != nil {
		return err
	}
	entries, err := os.ReadDir(v.Mount)
	if err != nil {
		return err
	}
	if len(entries) != 0 {
		return errors.New("postgres storage: mount would hide existing plaintext data")
	}
	if err := unix.Mount(v.device(), v.Mount, "ext4", unix.MS_NODEV|unix.MS_NOSUID, ""); err != nil {
		return errors.New("postgres storage: encrypted mount failed")
	}
	v.mounted = true
	v.state.Phase = "filesystem"
	if err := v.save(); err != nil {
		return err
	}
	return nil
}
func (v *Volume) closeMapping() error {
	if v.mounted {
		if err := unix.Unmount(v.Mount, 0); err != nil {
			return errors.New("postgres storage: encrypted filesystem still busy")
		}
		v.mounted = false
	}
	if v.mapped {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		if _, err := publicCommand(ctx, "cryptsetup", "close", v.mapping()); err != nil {
			return err
		}
		v.mapped = false
	}
	if v.loop != nil {
		_ = v.loop.Close()
		v.loop = nil
	}
	return nil
}
func (v *Volume) Close() error {
	err := v.closeMapping()
	if v.owner != nil {
		_ = v.owner.Close()
		v.owner = nil
	}
	return err
}
func (v *Volume) serveKey(ctx context.Context, conn *net.UnixConn) error {
	defer conn.Close()
	stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
	defer stop()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))
	if err := peerUID(conn, 1000); err != nil {
		return err
	}
	v.mu.Lock()
	defer v.mu.Unlock()
	request := make([]byte, requestSize)
	copy(request, protocol)
	copy(request[5:], v.state.UUID)
	if v.ready {
		request[4] = 2
	} else if v.unlocked {
		request[4] = 3
	} else if v.needsInitialize() {
		request[4] = 1
	}
	if _, err := conn.Write(request); err != nil {
		return err
	}
	if v.ready || v.unlocked {
		return nil
	}
	key, err := newSecret()
	if err != nil {
		return err
	}
	defer key.close()
	if _, err := io.ReadFull(conn, key.bytes()); err != nil {
		return errors.New("postgres storage: no Vault key received")
	}
	if err := v.unlock(ctx, key); err != nil {
		_ = v.closeMapping()
		return err
	}
	v.unlocked = true
	_, err = conn.Write([]byte{1})
	return err
}

// Run serves only the local unlock handshake, then starts the existing secure
// PostgreSQL entrypoint. Cancellation stops PostgreSQL before unmounting LUKS.
func (v *Volume) Run(ctx context.Context, command []string) (runErr error) {
	if os.Geteuid() != 0 {
		return errors.New("postgres storage: container storage administration requires root")
	}
	ctx, cancelServer := context.WithCancel(ctx)
	defer cancelServer()
	if len(command) == 0 {
		return errors.New("postgres storage: database command required")
	}
	if err := v.Prepare(); err != nil {
		_ = v.Close()
		return err
	}
	defer func() {
		if err := v.Close(); err != nil && runErr == nil {
			runErr = err
		}
	}()
	if err := os.MkdirAll(filepath.Dir(v.Socket), 0755); err != nil {
		return err
	}
	if st, err := os.Lstat(v.Socket); err == nil {
		if st.Mode()&os.ModeSocket == 0 {
			return errors.New("postgres storage: invalid control socket")
		}
		if err := os.Remove(v.Socket); err != nil {
			return err
		}
	}
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: v.Socket, Net: "unix"})
	if err != nil {
		return err
	}
	defer listener.Close()
	if err := os.Chmod(v.Socket, 0666); err != nil {
		return err
	}
	unlocked := make(chan struct{}, 1)
	serverDone := make(chan struct{})
	defer func() { cancelServer(); _ = listener.Close(); <-serverDone }()
	go func() {
		defer close(serverDone)
		for {
			conn, err := listener.AcceptUnix()
			if err != nil {
				return
			}
			attempt, cancel := context.WithTimeout(ctx, 30*time.Second)
			err = v.serveKey(attempt, conn)
			cancel()
			if err != nil {
				fmt.Fprintln(os.Stderr, "postgres storage: unlock denied; database remains locked:", err)
				continue
			}
			v.mu.Lock()
			ready := v.unlocked
			v.mu.Unlock()
			if ready {
				select {
				case unlocked <- struct{}{}:
				default:
				}
			}
		}
	}()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-unlocked:
	}
	if err := v.migrateLegacy(ctx); err != nil {
		return err
	}
	cmd := exec.Command(command[0], command[1:]...)
	cmd.Env = os.Environ()
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		return err
	}
	v.mu.Lock()
	v.ready = true
	v.mu.Unlock()
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	select {
	case err := <-done:
		return err
	case <-ctx.Done():
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM)
		select {
		case <-done:
			return nil
		case <-time.After(45 * time.Second):
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			<-done
			return errors.New("postgres storage: database forced to stop")
		}
	}
}
