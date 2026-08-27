package db1test

import (
	"os"
	"path/filepath"
	"testing"
)

func TestRebindNumbersOnlyUnquotedPlaceholders(t *testing.T) {
	t.Parallel()
	query := `UPDATE jobs SET detail='literal ? and ''quoted ?''' WHERE id=? AND owner=?`
	want := `UPDATE jobs SET detail='literal ? and ''quoted ?''' WHERE id=$1 AND owner=$2`
	if got := rebind(query); got != want {
		t.Fatalf("rebind=%q want %q", got, want)
	}
}

func TestCopyExecutableCreatesPrivateModuleAlias(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	source := filepath.Join(dir, "aimee-module")
	destination := filepath.Join(dir, "aimee-module-postgres")
	if err := os.WriteFile(source, []byte("module"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := copyExecutable(source, destination); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(destination)
	if err != nil {
		t.Fatal(err)
	}
	if string(body) != "module" {
		t.Fatalf("alias body=%q", body)
	}
	info, err := os.Stat(destination)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o700 {
		t.Fatalf("alias mode=%#o want 0700", got)
	}
}
