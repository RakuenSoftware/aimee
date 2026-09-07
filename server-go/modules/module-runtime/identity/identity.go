// Package identity provides the core's first-boot instance latch. Connection
// settings (including kb_mode) are independent of this immutable node identity.
package identity

import (
	"bytes"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const FileName = "instance-identity.json"
const Server = "server"
const KB = "kb"

type Identity struct {
	Version int    `json:"version"`
	Role    string `json:"role"`
	ID      string `json:"id"`
}

func (i Identity) valid() bool {
	if i.Version != 1 || (i.Role != Server && i.Role != KB) || len(i.ID) != 36 {
		return false
	}
	for n, c := range i.ID {
		if n == 8 || n == 13 || n == 18 || n == 23 {
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
func Read(home string) (Identity, error) {
	var result Identity
	if !filepath.IsAbs(home) {
		return result, errors.New("instance identity requires an absolute home")
	}
	path := filepath.Join(home, FileName)
	info, err := os.Lstat(path)
	if err != nil {
		return result, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm() != 0444 || info.Size() > 512 {
		return result, errors.New("invalid instance identity file")
	}
	file, err := os.Open(path)
	if err != nil {
		return result, err
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		return result, errors.New("instance identity changed while opening")
	}
	data, err := io.ReadAll(io.LimitReader(file, 513))
	if err != nil {
		return result, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&result) != nil || decoder.Decode(new(any)) != io.EOF || !result.valid() {
		return Identity{}, errors.New("invalid instance identity record")
	}
	canonical, err := json.Marshal(result)
	if err != nil || !bytes.Equal(data, append(canonical, '\n')) {
		return Identity{}, errors.New("noncanonical instance identity record")
	}
	return result, nil
}

// Ensure publishes a fully synced read-only record without replacing anything.
// Concurrent first boots converge on the winner; a different requested role
// is a startup error. Corruption, a symlink, or an unreadable file is not first boot.
func Ensure(home, role string) (Identity, error) {
	if !filepath.IsAbs(home) || (role != Server && role != KB) {
		return Identity{}, errors.New("invalid first-boot identity request")
	}
	if current, err := Read(home); err == nil {
		if current.Role != role {
			return Identity{}, fmt.Errorf("instance is %s; changing its identity to %s is forbidden", current.Role, role)
		}
		return current, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return Identity{}, err
	}
	if err := os.MkdirAll(home, 0700); err != nil {
		return Identity{}, err
	}
	var random [16]byte
	if _, err := rand.Read(random[:]); err != nil {
		return Identity{}, err
	}
	random[6] = (random[6] & 15) | 64
	random[8] = (random[8] & 63) | 128
	proposed := Identity{Version: 1, Role: role, ID: fmt.Sprintf("%x-%x-%x-%x-%x", random[:4], random[4:6], random[6:8], random[8:10], random[10:])}
	data, err := json.Marshal(proposed)
	if err != nil {
		return Identity{}, err
	}
	data = append(data, '\n')
	file, err := os.CreateTemp(home, ".instance-identity-")
	if err != nil {
		return Identity{}, err
	}
	defer os.Remove(file.Name())
	_, err = io.Copy(file, bytes.NewReader(data))
	if err == nil {
		err = file.Chmod(0444)
	}
	if err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return Identity{}, err
	}
	if closeErr != nil {
		return Identity{}, closeErr
	}
	err = os.Link(file.Name(), filepath.Join(home, FileName))
	if err != nil && !errors.Is(err, os.ErrExist) {
		return Identity{}, err
	}
	directory, err := os.Open(home)
	if err != nil {
		return Identity{}, err
	}
	err = directory.Sync()
	directory.Close()
	if err != nil {
		return Identity{}, err
	}
	current, err := Read(home)
	if err != nil {
		return Identity{}, err
	}
	if current.Role != role {
		return Identity{}, fmt.Errorf("instance is %s; changing its identity to %s is forbidden", current.Role, role)
	}
	return current, nil
}

// Bootstrap belongs to the role module's pre-bus startup. It writes only the
// non-secret identity latch, never configuration values or credential files.
func Bootstrap(args []string) (bool, int) {
	if len(args) < 2 || (args[1] != "__aimee_instance_bootstrap" && args[1] != "__aimee_instance_read") {
		return false, 0
	}
	if len(args) != 3 {
		return true, 1
	}
	var result Identity
	var err error
	if args[1] == "__aimee_instance_read" {
		result, err = Read(args[2])
		if errors.Is(err, os.ErrNotExist) {
			return true, 2
		}
	} else {
		role := ""
		switch filepath.Base(args[0]) {
		case "aimee-module-server":
			role = Server
		case "aimee-module-kb":
			role = KB
		}
		result, err = Ensure(args[2], role)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return true, 1
	}
	fmt.Fprintln(os.Stdout, result.Role)
	return true, 0
}
