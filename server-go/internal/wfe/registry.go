package wfe

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"syscall"
)

var versionSnapshotPattern = regexp.MustCompile(`\.v[0-9a-f]{64}\.yaml$`)

type DefinitionRow struct {
	Name    string `json:"name"`
	Valid   bool   `json:"valid"`
	Version string `json:"version"`
}

type DefinitionReport struct {
	Valid     bool       `json:"valid"`
	Name      string     `json:"name"`
	Version   string     `json:"version"`
	Canonical string     `json:"canonical"`
	Def       Definition `json:"def"`
	Error     string     `json:"error,omitempty"`
}

type VersionConflictError struct{ Current string }

func (e *VersionConflictError) Error() string { return "workflow version conflict" }

// Registry is the single definition authority shared by the GUI API, trigger
// admission, and execution. It reads definitions on every operation, so an
// atomic GUI save becomes live without a second cache or reload signal.
type Registry struct{ dir string }

var registryWriteMu sync.Mutex

func (r *Registry) lockWrites() (func(), error) {
	registryWriteMu.Lock()
	lock, err := os.OpenFile(filepath.Join(r.dir, ".registry.lock"), os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		registryWriteMu.Unlock()
		return nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX); err != nil {
		lock.Close()
		registryWriteMu.Unlock()
		return nil, err
	}
	return func() { _ = syscall.Flock(int(lock.Fd()), syscall.LOCK_UN); _ = lock.Close(); registryWriteMu.Unlock() }, nil
}

func NewRegistry(dir string) (*Registry, error) {
	if dir == "" {
		return nil, errors.New("workflow directory is required")
	}
	abs, err := filepath.Abs(dir)
	if err != nil {
		return nil, fmt.Errorf("resolve workflow directory: %w", err)
	}
	if err := os.MkdirAll(abs, 0o700); err != nil {
		return nil, fmt.Errorf("create workflow directory: %w", err)
	}
	return &Registry{dir: abs}, nil
}

func SafeName(name string) bool {
	if name == "" || len(name) >= 64 || name == "." || name == ".." || strings.Contains(name, "..") {
		return false
	}
	for _, r := range name {
		if !((r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '.' || r == '_' || r == '-') {
			return false
		}
	}
	return true
}

func (r *Registry) path(name string) (string, error) {
	if !SafeName(name) {
		return "", errors.New("invalid workflow name")
	}
	return filepath.Join(r.dir, name+".yaml"), nil
}

func (r *Registry) Load(name string) (Definition, error) {
	path, err := r.path(name)
	if err != nil {
		return Definition{}, err
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return Definition{}, err
	}
	report := r.Validate(content)
	if !report.Valid {
		return Definition{}, errors.New(report.Error)
	}
	return report.Def, nil
}

// LoadVersion resolves the immutable definition pinned when a work item was
// admitted. Editing the named workflow cannot alter or strand an active run.
func (r *Registry) LoadVersion(name, version string) (Definition, error) {
	if version == "" {
		return r.Load(name)
	}
	if !SafeName(name) || !isHash(version) {
		return Definition{}, errors.New("invalid workflow version")
	}
	content, err := os.ReadFile(filepath.Join(r.dir, name+".v"+version+".yaml"))
	if errors.Is(err, os.ErrNotExist) {
		// Upgrade compatibility: definitions installed before version snapshots
		// remain runnable when the current named content is exactly the pin.
		path, pathErr := r.path(name)
		if pathErr == nil {
			if named, readErr := os.ReadFile(path); readErr == nil {
				report := Report(named)
				if report.Valid && report.Version == version {
					catalog, catalogErr := r.Catalog()
					if catalogErr == nil && report.Def.ValidateCatalog(catalog) == nil {
						report.Def.Version = version
						return report.Def, nil
					}
				}
			}
		}
		def, loadErr := r.Load(name)
		if loadErr == nil && def.Version != version {
			return Definition{}, errors.New("pinned workflow definition is unavailable")
		}
		return def, loadErr
	}
	if err != nil {
		return Definition{}, err
	}
	report := Report(content)
	if !report.Valid {
		return Definition{}, errors.New(report.Error)
	}
	rawVersion := report.Version
	catalog, err := r.catalogVersion(name, version)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			return Definition{}, err
		}
		catalog, err = r.Catalog()
		if err != nil {
			return Definition{}, err
		}
		if rawVersion != version {
			return Definition{}, errors.New("pinned workflow catalog is unavailable")
		}
		if err := report.Def.ValidateCatalog(catalog); err != nil {
			return Definition{}, err
		}
		report.Def.Version = version
		return report.Def, nil
	}
	if err := report.Def.ValidateCatalog(catalog); err != nil {
		return Definition{}, err
	}
	applyCatalogVersion(&report, catalog)
	if report.Version != version {
		return Definition{}, errors.New("workflow version content mismatch")
	}
	return report.Def, nil
}

func isHash(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, c := range value {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

func (r *Registry) List() ([]DefinitionRow, error) {
	entries, err := os.ReadDir(r.dir)
	if err != nil {
		return nil, fmt.Errorf("list workflows: %w", err)
	}
	rows := make([]DefinitionRow, 0, len(entries))
	for _, entry := range entries {
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".yaml" || entry.Name() == "blocks.yaml" || versionSnapshotPattern.MatchString(entry.Name()) {
			continue
		}
		name := strings.TrimSuffix(entry.Name(), ".yaml")
		row := DefinitionRow{Name: name}
		if def, loadErr := r.Load(name); loadErr == nil {
			row.Valid, row.Version = true, def.Version
		}
		rows = append(rows, row)
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].Name < rows[j].Name })
	return rows, nil
}

func Report(content []byte) DefinitionReport {
	def, err := ParseDefinition(content)
	if err != nil {
		return DefinitionReport{Valid: false, Error: err.Error()}
	}
	canonical, err := CanonicalDefinition(def)
	if err != nil {
		return DefinitionReport{Valid: false, Name: def.Name, Error: err.Error()}
	}
	return DefinitionReport{Valid: true, Name: def.Name, Version: def.Version,
		Canonical: string(canonical), Def: def}
}

func (r *Registry) Validate(content []byte) DefinitionReport {
	report := Report(content)
	if report.Valid {
		catalog, catalogErr := r.Catalog()
		if catalogErr != nil {
			report.Valid, report.Error = false, catalogErr.Error()
			return report
		}
		if err := report.Def.ValidateCatalog(catalog); err != nil {
			report.Valid, report.Error = false, err.Error()
		} else {
			applyCatalogVersion(&report, catalog)
		}
	}
	return report
}

func applyCatalogVersion(report *DefinitionReport, catalog map[string]BlockDefinition) {
	used := make(map[string]BlockDefinition)
	for _, node := range report.Def.Nodes {
		if block, ok := catalog[node.Block]; ok {
			used[node.Block] = block
		}
	}
	encoded, _ := json.Marshal(used)
	report.Version = Hash(append(append([]byte{}, report.Canonical...), encoded...))
	report.Def.Version = report.Version
}

func (r *Registry) catalogVersion(name, version string) (map[string]BlockDefinition, error) {
	content, err := os.ReadFile(filepath.Join(r.dir, name+".v"+version+".blocks.json"))
	if err != nil {
		return nil, err
	}
	var catalog map[string]BlockDefinition
	if err := json.Unmarshal(content, &catalog); err != nil {
		return nil, err
	}
	return catalog, nil
}

func (r *Registry) BlockVersion(name, version, blockName string) (BlockDefinition, error) {
	catalog, err := r.catalogVersion(name, version)
	if errors.Is(err, os.ErrNotExist) {
		catalog, err = r.Catalog()
	}
	if err != nil {
		return BlockDefinition{}, err
	}
	block, ok := catalog[blockName]
	if !ok {
		return BlockDefinition{}, fmt.Errorf("block %q is unavailable", blockName)
	}
	return block, nil
}

// Pin publishes the current named workflow and resolved block catalog as one
// immutable execution bundle before admission returns its version.
func (r *Registry) Pin(name string) (Definition, error) {
	unlock, err := r.lockWrites()
	if err != nil {
		return Definition{}, err
	}
	defer unlock()
	path, err := r.path(name)
	if err != nil {
		return Definition{}, err
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return Definition{}, err
	}
	report := r.Validate(content)
	if !report.Valid {
		return Definition{}, errors.New(report.Error)
	}
	if err := r.writeSnapshot(report); err != nil {
		return Definition{}, err
	}
	return report.Def, nil
}

func (r *Registry) writeSnapshot(report DefinitionReport) error {
	versionPath := filepath.Join(r.dir, report.Name+".v"+report.Version+".yaml")
	blocksPath := filepath.Join(r.dir, report.Name+".v"+report.Version+".blocks.json")
	catalog, err := r.Catalog()
	if err != nil {
		return err
	}
	used := make(map[string]BlockDefinition)
	for _, node := range report.Def.Nodes {
		used[node.Block] = catalog[node.Block]
	}
	encoded, err := json.Marshal(used)
	if err != nil {
		return err
	}
	if _, err := os.Stat(blocksPath); errors.Is(err, os.ErrNotExist) {
		if err := atomicWrite(blocksPath, r.dir, report.Name, encoded); err != nil {
			return err
		}
	}
	if _, err := os.Stat(versionPath); errors.Is(err, os.ErrNotExist) {
		if err := atomicWrite(versionPath, r.dir, report.Name, []byte(report.Canonical)); err != nil {
			return err
		}
	}
	return nil
}

func (r *Registry) Report(name string) (DefinitionReport, error) {
	path, err := r.path(name)
	if err != nil {
		return DefinitionReport{}, err
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return DefinitionReport{}, err
	}
	return r.Validate(content), nil
}

func CanonicalDefinition(def Definition) ([]byte, error) {
	copy := def
	copy.Version = ""
	content, err := canonicalYAML(copy)
	if err != nil {
		return nil, fmt.Errorf("canonicalize workflow: %w", err)
	}
	return content, nil
}

func (r *Registry) Save(name string, content []byte, previousVersion string) (DefinitionReport, error) {
	unlock, err := r.lockWrites()
	if err != nil {
		return DefinitionReport{}, fmt.Errorf("lock workflow registry: %w", err)
	}
	defer unlock()
	path, err := r.path(name)
	if err != nil {
		return DefinitionReport{}, err
	}
	report := r.Validate(content)
	if !report.Valid {
		return report, errors.New(report.Error)
	}
	if report.Name != name {
		return report, errors.New("workflow body name does not match save name")
	}
	current := ""
	_, statErr := os.Stat(path)
	exists := statErr == nil
	if exists {
		if existing, loadErr := r.Load(name); loadErr == nil {
			current = existing.Version
		}
	}
	if (!exists && previousVersion != "") || (exists && previousVersion != current) {
		return report, &VersionConflictError{Current: current}
	}
	if err := r.writeSnapshot(report); err != nil {
		return report, err
	}
	if err := atomicWrite(path, r.dir, name, []byte(report.Canonical)); err != nil {
		return report, err
	}
	return report, nil
}

func atomicWrite(path, dir, name string, content []byte) error {
	tmp, err := os.CreateTemp(dir, "."+name+".*.tmp")
	if err != nil {
		return fmt.Errorf("create workflow temp file: %w", err)
	}
	tmpPath := tmp.Name()
	defer os.Remove(tmpPath)
	if err := tmp.Chmod(0o600); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := writeAndSync(tmp, content); err != nil {
		_ = tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpPath, path); err != nil {
		return fmt.Errorf("publish workflow: %w", err)
	}
	if err := syncDir(dir); err != nil {
		return err
	}
	return nil
}
