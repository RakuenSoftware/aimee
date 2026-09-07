package db_test

import (
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// The shared contract must not become a second provider or acquire a dependency
// on one of its domain consumers. Check imports, not comments or package aliases.
func TestSharedDatabaseContractHasNoProviderOrDomainDependency(t *testing.T) {
	checkImports(t, ".", func(path string) bool {
		return strings.Contains(strings.Split(path, "/")[0], ".")
	})
}

func TestMemoryDoesNotImportServerDomainStorage(t *testing.T) {
	checkImports(t, "../modules/memory", func(path string) bool {
		return path == "github.com/JBailes/aimee/server-go/modules/aimee" ||
			strings.HasPrefix(path, "github.com/JBailes/aimee/server-go/modules/aimee/")
	})
}

func TestRuntimeCallersDoNotReintroduceNumberedDatabasePackages(t *testing.T) {
	err := filepath.WalkDir("..", func(path string, entry os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if entry.IsDir() || !strings.HasSuffix(path, ".go") {
			return nil
		}
		file, err := parser.ParseFile(token.NewFileSet(), path, nil, parser.ImportsOnly)
		if err != nil {
			return err
		}
		for _, imp := range file.Imports {
			name, err := strconv.Unquote(imp.Path.Value)
			if err != nil {
				return err
			}
			for _, retired := range []string{"github.com/JBailes/aimee/server-go/db1", "github.com/JBailes/aimee/server-go/internal/db1"} {
				if name == retired || strings.HasPrefix(name, retired+"/") {
					t.Errorf("%s imports retired runtime package %s", path, name)
				}
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}

func checkImports(t *testing.T, dir string, forbidden func(string) bool) {
	t.Helper()
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	scanned := 0
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".go") || strings.HasSuffix(entry.Name(), "_test.go") {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		file, err := parser.ParseFile(token.NewFileSet(), path, nil, parser.ImportsOnly)
		if err != nil {
			t.Fatal(err)
		}
		scanned++
		for _, imp := range file.Imports {
			name, err := strconv.Unquote(imp.Path.Value)
			if err != nil {
				t.Fatal(err)
			}
			if forbidden(name) {
				t.Errorf("%s imports forbidden dependency %s", path, name)
			}
		}
	}
	if scanned == 0 {
		t.Fatalf("no production sources checked in %s", dir)
	}
}
