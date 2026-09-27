package postgres

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

// The C integration fixture launches this same executable as a private provider.
func TestMain(m *testing.M) {
	if len(os.Args) == 2 && os.Args[1] == "__aimee_postgres_local_session" {
		if err := ServeLocalSession(context.Background(), os.Stdin, os.Stdout); err != nil {
			os.Exit(1)
		}
		os.Exit(0)
	}
	os.Exit(m.Run())
}
func TestLocalSessionFraming(t *testing.T) {
	for _, body := range [][]byte{nil, {0}, {0, 0, 0, 0}, {255, 255, 255, 255}, {2, 0, 0, 0, 1}} {
		if _, err := readLocalFrame(bytes.NewReader(body), 32); err == nil {
			t.Fatal("accepted malformed local frame")
		}
	}
	var out bytes.Buffer
	if err := writeLocalFrame(&out, []byte("abc")); err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint32(out.Bytes()) != 3 {
		t.Fatal("length")
	}
	got, err := readLocalFrame(&out, 3)
	if err != nil || string(got) != "abc" {
		t.Fatal("frame roundtrip", err)
	}
	if _, err := readLocalFrame(&out, 3); err != io.EOF {
		t.Fatal("EOF", err)
	}
}
func TestLocalSessionAuthoritySelection(t *testing.T) {
	for _, authority := range [][]byte{{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0, 'x'}, {3, 0, 0, 0, 'x'}, {4, 0, 0, 0}} {
		var input, output bytes.Buffer
		if err := writeLocalFrame(&input, authority); err != nil {
			t.Fatal(err)
		}
		if err := ServeLocalSession(context.Background(), &input, &output); err == nil {
			t.Fatal("invalid authority accepted")
		}
		if output.Len() != 0 {
			t.Fatal("invalid authority opened a session")
		}
	}
}

func TestPostgresLocalNativeSession(t *testing.T) {
	if os.Getenv("AIMEE_DB_TEST_URL") == "" {
		if os.Getenv("AIMEE_DB_TEST_REQUIRED") == "1" {
			t.Fatal("AIMEE_DB_TEST_URL required")
		}
		t.Skip("requires disposable PostgreSQL")
	}
	_, source, _, _ := runtime.Caller(0)
	root := filepath.Clean(filepath.Join(filepath.Dir(source), "../../.."))
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	target := filepath.Join(t.TempDir(), "native-postgres-session")
	args := []string{"-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
		"-DAIMEE_POSTGRES_LOCAL_PROVIDER=\"" + executable + "\"", "-o", target}
	for _, dir := range []string{"src", "src/modules/kb/c", "src/headers", "src/vendor/headers", "src/modules/postgres/include", "src/modules/audit/include", "src/core/event_bus/include"} {
		args = append(args, "-I"+filepath.Join(root, dir))
	}
	for _, file := range []string{"src/tests/test_postgres_local_session.c", "src/modules/kb/c/db_postgres.c", "src/modules/postgres/client/session.c", "src/modules/postgres/client/local_session.c"} {
		args = append(args, filepath.Join(root, file))
	}
	if output, err := exec.CommandContext(ctx, "cc", args...).CombinedOutput(); err != nil {
		t.Fatalf("native fixture compilation: %v\n%s", err, output)
	}
	if output, err := exec.CommandContext(ctx, target).CombinedOutput(); err != nil {
		t.Fatalf("native fixture: %v\n%s", err, output)
	}
}
