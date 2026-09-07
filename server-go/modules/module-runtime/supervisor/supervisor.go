// Package supervisor runs a role's independently restartable module processes.
// The bus remains the authority for executable identity, grants and access.
package supervisor

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
)

type Module struct{ ID, Executable string }

var moduleID = regexp.MustCompile(`^[a-z0-9]+(?:-[a-z0-9]+)*$`)

// Read validates the entire composition before any process can be launched.
// Both role compositions require the same configuration, storage and memory
// modules. Vault bootstrap runs before SQL and is supplied by the resource host.
func Read(home, role, manifest string) ([]Module, error) {
	instance, err := identity.Read(home)
	if err != nil || string(instance.Role) != role || (role != "server" && role != "kb") {
		return nil, errors.New("module composition conflicts with the first-boot identity")
	}
	file, err := os.Open(manifest)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	seen := make(map[string]bool)
	var modules []Module
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := scanner.Text()
		if line == "" {
			continue
		}
		parts := strings.Split(line, "\t")
		if len(parts) != 2 || !moduleID.MatchString(parts[0]) || !filepath.IsAbs(parts[1]) || seen[parts[0]] || len(modules) >= 256 {
			return nil, errors.New("invalid or duplicate module manifest entry")
		}
		if (parts[0] == "server" || parts[0] == "kb") && parts[0] != role {
			return nil, errors.New("a composition cannot attach both Server and KB")
		}
		info, err := os.Stat(parts[1])
		if err != nil || !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
			return nil, fmt.Errorf("module %s executable is unavailable", parts[0])
		}
		seen[parts[0]] = true
		modules = append(modules, Module{parts[0], parts[1]})
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	for _, required := range []string{role, "config", "postgres", "memory"} {
		if !seen[required] {
			return nil, fmt.Errorf("composition is missing required module %s", required)
		}
	}
	return modules, nil
}

func delay(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

// Run restarts a failed module independently and stops every child when the
// owning role terminates. A failed manifest is never partially activated.
func Run(ctx context.Context, home, role, socket, manifest string) error {
	modules, err := Read(home, role, manifest)
	if err != nil {
		return err
	}
	if !filepath.IsAbs(socket) {
		return errors.New("module bus socket must be absolute")
	}
	var workers sync.WaitGroup
	for _, module := range modules {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for ctx.Err() == nil {
				for ctx.Err() == nil {
					info, err := os.Stat(socket)
					if err == nil && info.Mode()&os.ModeSocket != 0 {
						break
					}
					if !delay(ctx, 100*time.Millisecond) {
						return
					}
				}
				if ctx.Err() != nil {
					return
				}
				log.Printf("composition:%s starting %s", role, module.ID)
				err := child(ctx, module.Executable, socket, role, home)
				if ctx.Err() != nil {
					return
				}
				log.Printf("composition:%s module %s exited (%v); restarting", role, module.ID, err)
				if !delay(ctx, time.Second) {
					return
				}
			}
		}()
	}
	workers.Wait()
	return nil
}
