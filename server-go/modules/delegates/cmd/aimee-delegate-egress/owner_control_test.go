package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestOwnerEngineEndpointExcludesAmbientOrForeignResources(t *testing.T) {
	accepted := fmt.Sprintf("unix:///proc/%d/fd/3", os.Getppid())
	if !ownerEngineEndpoint(accepted) {
		t.Fatal("held parent descriptor refused")
	}
	for _, value := range []string{"", "unix:///var/run/docker.sock", "tcp://127.0.0.1:2375", fmt.Sprintf("unix:///proc/%d/fd/3", os.Getpid()), strings.TrimSuffix(accepted, "3") + "03", accepted + "?context=other", strings.TrimSuffix(accepted, "3") + "2"} {
		if ownerEngineEndpoint(value) {
			t.Fatal("unowned daemon endpoint accepted", value)
		}
	}
}

func TestMeasuredHelperMustRemainTheSamePrivateFile(t *testing.T) {
	socket, _ := ownerSocket(t)
	path := filepath.Join(filepath.Dir(socket), "owner-io")
	content := []byte("owner-shipped-static-executable-fixture")
	hash := sha256.Sum256(content)
	digest := hex.EncodeToString(hash[:])
	write := func() {
		if err := os.WriteFile(path, content, 0555); err != nil {
			t.Fatal(err)
		}
	}
	write()
	for _, defect := range []string{"", "digest", "mode", "hardlink", "symlink", "source"} {
		t.Run(defect, func(t *testing.T) {
			expected, source := digest, path
			switch defect {
			case "digest":
				expected = strings.Repeat("0", 64)
			case "mode":
				os.Chmod(path, 0755)
				defer os.Chmod(path, 0555)
			case "hardlink":
				alias := path + ".link"
				if err := os.Link(path, alias); err != nil {
					t.Fatal(err)
				}
				defer os.Remove(alias)
			case "symlink":
				if err := os.Rename(path, path+".saved"); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(path+".saved", path); err != nil {
					t.Fatal(err)
				}
				defer func() { os.Remove(path); os.Rename(path+".saved", path) }()
			case "source":
				source = "/run/aimee/owner-io"
			}
			file, observed, err := measuredOwnerHelper(path, source, expected, socket, "")
			if file != nil {
				defer file.Close()
			}
			if defect == "" {
				if err != nil || observed != digest {
					t.Fatal("measured helper refused", err, observed)
				}
			} else if err == nil {
				t.Fatal("unmeasured helper accepted", defect)
			}
		})
	}
	file, _, err := measuredOwnerHelper(path, path, digest, socket, "")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	write()
	if _, err := verifyOwnerHelper(file, path, digest); err == nil {
		t.Fatal("replacement with same bytes retained identity")
	}
}
