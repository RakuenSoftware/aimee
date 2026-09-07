//go:build linux

package storage

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"golang.org/x/sys/unix"
)

const protocol = "APG1"
const requestSize = 41 // magic, state (0 existing/1 new/2 ready/3 preparing), volume UUID

func validVolumeID(id string) bool {
	if len(id) != 36 {
		return false
	}
	for i, c := range id {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
			continue
		}
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func peerUID(conn *net.UnixConn, uid uint32) error {
	raw, err := conn.SyscallConn()
	if err != nil {
		return err
	}
	var peerErr error
	if err := raw.Control(func(fd uintptr) {
		cred, e := unix.GetsockoptUcred(int(fd), unix.SOL_SOCKET, unix.SO_PEERCRED)
		if e != nil || cred.Uid != uid {
			peerErr = errors.New("postgres storage: unauthorized peer")
		}
	}); err != nil {
		return err
	}
	return peerErr
}

// vaultKey executes the core's restricted resource, not an arbitrary command or
// key provider. Neither side persists plaintext keys. The helper attests this
// installed postgres module and serves only its volume-bound Vault slot.
func vaultKey(ctx context.Context, home, volume string, initialize bool, key *secret) error {
	request := make([]byte, 37)
	if initialize {
		request[0] = 1
	}
	copy(request[1:], volume)
	cmd := exec.CommandContext(ctx, "/usr/local/bin/aimee-server", "--postgres-vault-resource")
	cmd.Env = []string{"AIMEE_HOME=" + home, "PATH=/usr/local/bin:/usr/bin:/bin"}
	cmd.Stdin = bytes.NewReader(request)
	cmd.Stderr = os.Stderr
	output, err := cmd.StdoutPipe()
	if err != nil {
		return errors.New("postgres storage: Vault unavailable")
	}
	if err = cmd.Start(); err != nil {
		return errors.New("postgres storage: Vault unavailable")
	}
	_, readErr := io.ReadFull(output, key.bytes())
	var extra [1]byte
	n, endErr := output.Read(extra[:])
	waitErr := cmd.Wait()
	if readErr != nil || n != 0 || endErr != io.EOF || waitErr != nil {
		clear(key.bytes())
		return errors.New("postgres storage: Vault refused the volume key")
	}
	return nil
}

// UnlockOnce is the postgres owner's pre-database bootstrap. The control
// socket is mounted read-only from the storage container; the root peer never
// sends key material. It only identifies the volume it needs unlocked.
func UnlockOnce(ctx context.Context, socket, home string) error {
	if !filepath.IsAbs(socket) || !filepath.IsAbs(home) {
		return errors.New("postgres storage: absolute paths required")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	// Use the Unix-only API: this resource path cannot become Internet egress.
	conn, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: socket, Net: "unix"})
	if err != nil {
		return errors.New("postgres storage: unlock service unavailable")
	}
	defer conn.Close()
	deadline := time.Now().Add(30 * time.Second)
	if d, ok := ctx.Deadline(); ok && d.Before(deadline) {
		deadline = d
	}
	_ = conn.SetDeadline(deadline)
	if err := peerUID(conn, 0); err != nil {
		return err
	}
	var request [requestSize]byte
	if _, err := io.ReadFull(conn, request[:]); err != nil {
		return errors.New("postgres storage: invalid unlock request")
	}
	volume := string(request[5:])
	if string(request[:4]) != protocol || request[4] > 3 || !validVolumeID(volume) {
		return errors.New("postgres storage: invalid volume identity")
	}
	if request[4] == 2 {
		return nil
	}
	if request[4] == 3 {
		return errors.New("postgres storage: preparing database")
	}
	key, err := newSecret()
	if err != nil {
		return err
	}
	defer key.close()
	if err := vaultKey(ctx, home, volume, request[4] == 1, key); err != nil {
		return err
	}
	if _, err := conn.Write(key.bytes()); err != nil {
		return errors.New("postgres storage: key handoff failed")
	}
	clear(key.bytes())
	var result [1]byte
	if _, err := io.ReadFull(conn, result[:]); err != nil || result[0] != 1 {
		return errors.New("postgres storage: encrypted mount unavailable")
	}
	return errors.New("postgres storage: preparing database")
}

// WaitForUnlock is bounded so a missing Vault/storage service prevents startup.
func WaitForUnlock(ctx context.Context, socket, home string) error {
	var last error
	for {
		if last = UnlockOnce(ctx, socket, home); last == nil {
			return nil
		}
		select {
		case <-ctx.Done():
			return last
		case <-time.After(time.Second):
		}
	}
}

// MaintainUnlock retries after an independently restarted database container.
// Errors never cause creation of another key or a plaintext database fallback.
func MaintainUnlock(ctx context.Context, socket, home string) {
	for {
		attempt, cancel := context.WithTimeout(ctx, 30*time.Second)
		_ = UnlockOnce(attempt, socket, home)
		cancel()
		select {
		case <-ctx.Done():
			return
		case <-time.After(2 * time.Second):
		}
	}
}

func Bootstrap(args []string) (bool, int) {
	if len(args) < 2 || args[1] != "__aimee_postgres_unlock" {
		return false, 0
	}
	if len(args) != 3 || filepath.Base(args[0]) != "aimee-module-postgres" {
		return true, 1
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Minute)
	defer cancel()
	if err := unix.Setrlimit(unix.RLIMIT_CORE, &unix.Rlimit{}); err != nil {
		return true, 1
	}
	if err := WaitForUnlock(ctx, args[2], os.Getenv("AIMEE_HOME")); err != nil {
		_, _ = io.WriteString(os.Stderr, err.Error()+"\n")
		return true, 1
	}
	return true, 0
}
