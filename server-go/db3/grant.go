package db3

import (
	"bufio"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Is a vector database installed?
//
// The answer is a FACT ON DISK, not a conversation. An operator provisions a
// provider, which writes db3-<instance>.grant into the daemon's policy
// directory, and the bus host reads that directory once at boot. Either a
// module has taken the grant or none has.
//
// This replaces a registry that learned about providers from announcements over
// the bus. That machinery was answering a question nobody asked: a provider
// cannot serve without a grant, the grant cannot appear while the daemon is
// running -- grants load once, at start -- and a module that has to discover
// its own deployment is a module that can be wrong about it. Reading the same
// directory the host read is one source of truth for both.

// ProviderGrant is a provisioned vector provider.
type ProviderGrant struct {
	// Instance is the name from db3-<instance>.grant.
	Instance string
	// PrincipalRef is the ref allocated from the provider band.
	PrincipalRef uint32
	// Executable is the binary the bus will admit under this grant. A provider
	// running from anywhere else is refused at attach, so this is what the
	// deployment actually authorised rather than what is installed.
	Executable string
}

// grantPrefix is what the provisioner names a DB3 provider grant.
const grantPrefix = "db3-"

// ProviderGrants reports the vector providers this deployment has provisioned.
//
// An empty result is the ORDINARY case: no vector database installed, every
// vector operation served in-database. It is never an error, and a missing or
// unreadable policy directory is the same answer -- a deployment without one has
// provisioned nothing.
func ProviderGrants(policyDir string) []ProviderGrant {
	if strings.TrimSpace(policyDir) == "" {
		return nil
	}
	entries, err := os.ReadDir(policyDir)
	if err != nil {
		return nil
	}
	var grants []ProviderGrant
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() || !strings.HasPrefix(name, grantPrefix) ||
			!strings.HasSuffix(name, ".grant") {
			continue
		}
		grant, ok := readProviderGrant(filepath.Join(policyDir, name))
		if !ok {
			continue
		}
		grant.Instance = strings.TrimSuffix(strings.TrimPrefix(name, grantPrefix), ".grant")
		grants = append(grants, grant)
	}
	return grants
}

// readProviderGrant parses one grant file.
//
// A grant whose ref is outside the reserved provider band is IGNORED rather
// than trusted. The band is what keeps a provider from deriving a canonical
// module's event kinds, and a file that names a ref outside it was either
// hand-edited or written by a provisioner that predates the band -- neither is
// something to route vector traffic through.
func readProviderGrant(path string) (ProviderGrant, bool) {
	file, err := os.Open(path)
	if err != nil {
		return ProviderGrant{}, false
	}
	defer file.Close()

	var grant ProviderGrant
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		key, value, found := strings.Cut(strings.TrimSpace(scanner.Text()), "=")
		if !found {
			continue
		}
		switch strings.TrimSpace(key) {
		case "principal_ref":
			ref, err := strconv.ParseUint(strings.TrimSpace(value), 10, 32)
			if err != nil {
				return ProviderGrant{}, false
			}
			grant.PrincipalRef = uint32(ref)
		case "executable":
			grant.Executable = strings.TrimSpace(value)
		}
	}
	if scanner.Err() != nil || ValidateProviderRef(grant.PrincipalRef) != nil {
		return ProviderGrant{}, false
	}
	return grant, true
}
