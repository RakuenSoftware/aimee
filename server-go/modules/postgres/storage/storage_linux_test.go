//go:build linux

package storage

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"unsafe"
)

func fixtureVolume(t *testing.T) *Volume {
	t.Helper()
	root := t.TempDir()
	return &Volume{Root: filepath.Join(root, "ciphertext"), Mount: filepath.Join(root, "mounted"), Socket: filepath.Join(root, "control", "storage.sock"), Size: 256 << 20}
}
func TestPreparePersistsIdentityAndExcludesConcurrentOwner(t *testing.T) {
	v := fixtureVolume(t)
	if err := v.Prepare(); err != nil {
		t.Fatal(err)
	}
	id := v.state.UUID
	if !validVolumeID(id) || !v.needsInitialize() {
		t.Fatal("fresh volume identity missing")
	}
	other := &Volume{Root: v.Root, Mount: v.Mount, Socket: v.Socket, Size: v.Size}
	if err := other.Prepare(); err == nil {
		other.Close()
		t.Fatal("second owner admitted")
	}
	if err := v.Close(); err != nil {
		t.Fatal(err)
	}
	if err := other.Prepare(); err != nil {
		t.Fatal(err)
	}
	defer other.Close()
	if other.state.UUID != id {
		t.Fatal("restart changed volume identity")
	}
}
func TestPrepareNeverAdoptsPlaintextOrUnrecognizedData(t *testing.T) {
	for _, name := range []string{"PG_VERSION", "postgres.luks", ".manifest-interrupted"} {
		t.Run(name, func(t *testing.T) {
			v := fixtureVolume(t)
			if err := os.MkdirAll(v.Root, 0700); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(v.Root, name)
			canary := []byte("EXISTING-DATA-MUST-SURVIVE")
			if err := os.WriteFile(path, canary, 0600); err != nil {
				t.Fatal(err)
			}
			if err := v.Prepare(); err == nil {
				t.Fatal("existing storage adopted")
			}
			v.Close()
			got, err := os.ReadFile(path)
			if err != nil || string(got) != string(canary) {
				t.Fatal("existing storage changed")
			}
		})
	}
}
func TestMissingCommittedVolumeCannotBecomeFresh(t *testing.T) {
	v := fixtureVolume(t)
	if err := v.Prepare(); err != nil {
		t.Fatal(err)
	}
	v.state.Phase = "filesystem"
	if err := v.save(); err != nil {
		t.Fatal(err)
	}
	v.Close()
	if err := v.Prepare(); err == nil {
		t.Fatal("missing committed volume became fresh")
	}
	v.Close()
}
func TestManifestCorruptionAndResizeFailClosed(t *testing.T) {
	for _, variant := range []string{"truncated", "trailing", "wrong-version", "unknown-field", "resize"} {
		t.Run(variant, func(t *testing.T) {
			v := fixtureVolume(t)
			if err := v.Prepare(); err != nil {
				t.Fatal(err)
			}
			v.Close()
			path := filepath.Join(v.Root, "volume.json")
			data, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			switch variant {
			case "truncated":
				data = []byte("{broken")
			case "trailing":
				data = append(data, []byte("{}")...)
			case "wrong-version":
				v.state.Version = 2
				data, _ = json.Marshal(v.state)
			case "unknown-field":
				data = append(data[:len(data)-1], []byte(",\"key_file\":\"/tmp/key\"}")...)
			case "resize":
				v.Size *= 2
			}
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			if err := v.Prepare(); err == nil {
				t.Fatal("invalid storage configuration accepted")
			}
			v.Close()
			got, _ := os.ReadFile(path)
			if string(got) != string(data) {
				t.Fatal("manifest replaced")
			}
		})
	}
}
func TestVolumeRefusesSymlinkImage(t *testing.T) {
	v := fixtureVolume(t)
	if err := v.Prepare(); err != nil {
		t.Fatal(err)
	}
	v.Close()
	target := filepath.Join(t.TempDir(), "untouched")
	if err := os.WriteFile(target, []byte("CANARY"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, v.image()); err != nil {
		t.Fatal(err)
	}
	if err := v.Prepare(); err == nil {
		t.Fatal("symlink accepted")
	}
	v.Close()
	got, _ := os.ReadFile(target)
	if string(got) != "CANARY" {
		t.Fatal("target changed")
	}
}
func TestSecretMappingIsLockedAndNonDumpable(t *testing.T) {
	s, err := newSecret()
	if err != nil {
		t.Fatal(err)
	}
	defer s.close()
	if len(s.bytes()) != 32 {
		t.Fatal("incorrect key size")
	}
	for i := range s.bytes() {
		s.bytes()[i] = byte(i + 1)
	}
	// Linux reports the relevant mapping flags in smaps. This is a real memory
	// protection check; it does not substitute a heap buffer when mlock fails.
	data, err := os.ReadFile("/proc/self/smaps")
	if err != nil {
		t.Fatal(err)
	}
	addr := uint64(uintptr(unsafe.Pointer(&s.page[0])))
	scanner := bufio.NewScanner(bytes.NewReader(data))
	active, verified := false, false
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 {
			continue
		}
		if bounds := strings.Split(fields[0], "-"); len(bounds) == 2 {
			low, e1 := strconv.ParseUint(bounds[0], 16, 64)
			high, e2 := strconv.ParseUint(bounds[1], 16, 64)
			active = e1 == nil && e2 == nil && low <= addr && addr < high
		}
		if active && fields[0] == "VmFlags:" {
			flags := " " + strings.Join(fields[1:], " ") + " "
			if !strings.Contains(flags, " lo ") || !strings.Contains(flags, " dd ") {
				t.Fatal("secret mapping is swappable or dumpable")
			}
			verified = true
			break
		}
	}
	if !verified {
		t.Fatal("secret mapping absent from smaps")
	}
}
func TestUnlockRefusesUnavailableService(t *testing.T) {
	if err := UnlockOnce(context.Background(), filepath.Join(t.TempDir(), "missing.sock"), t.TempDir()); err == nil {
		t.Fatal("missing unlock service accepted")
	}
}
