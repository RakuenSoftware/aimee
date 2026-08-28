//go:build linux

package main

import (
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/sys/unix"
)

func TestModuleNetworkGuardDeniesInternetAndRetainsTheBus(t *testing.T) {
	if os.Getenv("AIMEE_TEST_NETWORK_GUARD_HELPER") == "1" {
		if err := installModuleNetworkGuard(); err != nil {
			t.Fatal(err)
		}
		if listener, err := net.Listen("tcp4", "127.0.0.1:0"); err == nil {
			_ = listener.Close()
			t.Fatal("IPv4 socket creation survived the module network guard")
		}
		path := filepath.Join(t.TempDir(), "bus.sock")
		listener, err := net.Listen("unix", path)
		if err != nil {
			t.Fatalf("AF_UNIX must remain available for the module bus: %v", err)
		}
		_ = listener.Close()
		return
	}
	command := exec.Command(os.Args[0], "-test.run=^TestModuleNetworkGuardDeniesInternetAndRetainsTheBus$")
	command.Env = append(os.Environ(), "AIMEE_TEST_NETWORK_GUARD_HELPER=1")
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("network-guard subprocess: %v\n%s", err, strings.TrimSpace(string(output)))
	}
}

func TestEgressCredentialOwnerIsNonDumpable(t *testing.T) {
	if os.Getenv("AIMEE_TEST_EGRESS_DUMP_HELPER") == "1" {
		if err := hardenEgressCredentialOwner(); err != nil {
			t.Fatal(err)
		}
		value, _, errno := unix.Syscall(unix.SYS_PRCTL, unix.PR_GET_DUMPABLE, 0, 0)
		if errno != 0 || value != 0 {
			t.Fatalf("PR_GET_DUMPABLE = %d, errno=%v", value, errno)
		}
		return
	}
	command := exec.Command(os.Args[0], "-test.run=^TestEgressCredentialOwnerIsNonDumpable$")
	command.Env = append(os.Environ(), "AIMEE_TEST_EGRESS_DUMP_HELPER=1")
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("egress-hardening subprocess: %v\n%s", err, strings.TrimSpace(string(output)))
	}
}
