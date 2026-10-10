// Offline maintenance utility. The operator stops the target owner before
// import, retains erasures.json, and restarts it to clear derived provider state.
package main

import (
	"context"
	"flag"
	"fmt"
	"github.com/JBailes/aimee/server-go/modules/memory/backendstore"
	"io"
	"os"
)

func main() {
	operation := flag.String("operation", "", "export or import")
	directory := flag.String("directory", "", "absolute adapter catalog directory")
	namespace := flag.String("namespace", "", "host-derived namespace UUID")
	file := flag.String("file", "", "snapshot file (0600)")
	flag.Parse()
	fail := func() {
		fmt.Fprintln(os.Stderr, "memory transfer refused: check operation, permissions, snapshot validity, erasure controls and empty-target requirement")
		os.Exit(1)
	}
	if *file == "" || (*operation != "export" && *operation != "import") {
		fail()
	}
	catalog, err := backendstore.New(*directory, *namespace)
	if err != nil {
		fail()
	}
	ctx := context.Background()
	if *operation == "import" {
		f, e := os.Open(*file)
		if e != nil {
			fail()
		}
		raw, e := io.ReadAll(io.LimitReader(f, backendstore.MaxSnapshot+1))
		f.Close()
		if e != nil || catalog.Import(ctx, raw) != nil {
			fail()
		}
	} else {
		raw, e := catalog.Export(ctx)
		if e != nil {
			fail()
		}
		f, e := os.OpenFile(*file, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if e != nil {
			fail()
		}
		_, e = f.Write(raw)
		if e == nil {
			e = f.Sync()
		}
		closeErr := f.Close()
		if e != nil || closeErr != nil {
			os.Remove(*file)
			fail()
		}
	}
	fmt.Println("Memory transfer completed; restart the target memory owner before serving.")
}
