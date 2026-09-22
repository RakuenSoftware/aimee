// Generate schema seed and temporary native compatibility data from Go.
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/JBailes/aimee/server-go/modules/memory"
)

func main() {
	root := flag.String("root", "..", "repository root")
	check := flag.Bool("check", false, "check generated files without writing")
	db2 := flag.String("db2-output", "", "write only the DB2 compatibility file")
	flag.Parse()
	if flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "unexpected positional arguments")
		os.Exit(2)
	}
	artifacts, err := memory.OntologyArtifacts()
	if err != nil {
		fail(err)
	}
	if *db2 != "" {
		if err = os.WriteFile(*db2, []byte(artifacts["db2"]), 0644); err != nil {
			fail(err)
		}
		return
	}
	outputs := map[string]string{
		"server-go/modules/memory/testdata/ontology_seed.tsv": artifacts["tsv"],
		"src/modules/db2/support/rel_seed_primitives.c":       artifacts["db2"],
	}
	for _, item := range []struct{ path, start, end, key string }{
		{"src/rel_types.c", "/* BEGIN GO MEMORY ONTOLOGY SEED */", "/* END GO MEMORY ONTOLOGY SEED */", "native"},
		{"src/modules/db2/c/schema.sql", "-- BEGIN GO MEMORY ONTOLOGY SEED", "-- END GO MEMORY ONTOLOGY SEED", "sql"},
		{"src/modules/db2/c/schema_sqlite.sql", "-- BEGIN GO MEMORY ONTOLOGY SEED", "-- END GO MEMORY ONTOLOGY SEED", "sql"},
	} {
		raw, err := os.ReadFile(filepath.Join(*root, item.path))
		if err != nil {
			fail(err)
		}
		text := string(raw)
		a, b := strings.Index(text, item.start), strings.Index(text, item.end)
		if a < 0 || b <= a || strings.Count(text, item.start) != 1 || strings.Count(text, item.end) != 1 {
			fail(fmt.Errorf("invalid generated region in %s", item.path))
		}
		a += len(item.start)
		outputs[item.path] = text[:a] + "\n" + artifacts[item.key] + text[b:]
	}
	paths := make([]string, 0, len(outputs))
	for path := range outputs {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	// Resolve and validate all inputs before any writes.
	for _, path := range paths {
		full := filepath.Join(*root, path)
		if *check {
			raw, err := os.ReadFile(full)
			if err != nil {
				fail(err)
			}
			if string(raw) != outputs[path] {
				fail(fmt.Errorf("generated ontology drift: %s", path))
			}
			continue
		}
		if err := os.WriteFile(full, []byte(outputs[path]), 0644); err != nil {
			fail(err)
		}
	}
}
func fail(err error) { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
