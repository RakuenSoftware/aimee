package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Persist the managed login accounts so they survive the container being
// replaced.
//
// PAM identities live in the container's writable layer: /etc/passwd and
// /etc/shadow are on no volume. An image upgrade (`up --force-recreate`)
// therefore destroys every account the operator created, while $AIMEE_HOME —
// which holds their projects, keyed by webuser name — persists. The operator
// came back to an appliance that had lost the account their work was filed
// under. Minting a replacement login (the fix that stops the lockout) does not
// help with this: a NEW generated name leaves the existing project tree attached
// to a user nobody signs in as.
//
// So record the accounts, and restore them on the next boot.
//
// What is stored is the crypt(3) verifier, exactly what /etc/shadow already
// holds, 0600 and root-owned, on the volume that already stores the Vault. It is
// deliberately NOT sealed into the Vault: a host password is not one of aimee's
// own secrets, and check-webchat-bootstrap-login.sh pins that contract. Nor is a
// shadow verifier ever erased — the earlier design did both and broke PAM auth.
const identityRecordName = "identities"

// identityRecordPath is <home>/webchat/identities.
func identityRecordPath(home string) string {
	return filepath.Join(home, "webchat", identityRecordName)
}

// managedIdentity is one restorable account: the name and its shadow verifier.
type managedIdentity struct {
	Username string
	Hash     string
}

// usableShadowHash reports whether a shadow field is a verifier a human can
// authenticate against. Empty, "!"-locked and "*"-disabled are not: a retired
// bootstrap account keeps its group membership and loses only this, so restoring
// it would recreate an account nobody can sign in as.
func usableShadowHash(h string) bool {
	return h != "" && !strings.HasPrefix(h, "!") && !strings.HasPrefix(h, "*")
}

// readShadowHashes returns username -> verifier for every /etc/shadow entry.
func readShadowHashes(shadowPath string) (map[string]string, error) {
	f, err := os.Open(shadowPath)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	out := map[string]string{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		parts := strings.Split(sc.Text(), ":")
		if len(parts) >= 2 && parts[0] != "" {
			out[parts[0]] = parts[1]
		}
	}
	return out, sc.Err()
}

// renderIdentityRecord formats the restorable accounts, one "user:hash" per
// line, sorted so an unchanged set of accounts produces an unchanged file.
func renderIdentityRecord(ids []managedIdentity) string {
	sort.Slice(ids, func(i, j int) bool { return ids[i].Username < ids[j].Username })
	var b strings.Builder
	for _, id := range ids {
		if id.Username == "" || !usableShadowHash(id.Hash) {
			continue
		}
		// A colon or newline in either field would forge a second record.
		// Neither can occur in a real passwd/shadow field; skip rather than
		// write something the restore would misparse.
		if strings.ContainsAny(id.Username, ":\n") || strings.ContainsAny(id.Hash, ":\n") {
			continue
		}
		fmt.Fprintf(&b, "%s:%s\n", id.Username, id.Hash)
	}
	return b.String()
}

// parseIdentityRecord is the inverse, tolerating a truncated or empty file.
func parseIdentityRecord(raw string) []managedIdentity {
	var out []managedIdentity
	for _, line := range strings.Split(raw, "\n") {
		name, hash, ok := strings.Cut(strings.TrimSpace(line), ":")
		if !ok || name == "" || !usableShadowHash(hash) {
			continue
		}
		out = append(out, managedIdentity{Username: name, Hash: hash})
	}
	return out
}

// writeIdentityRecord replaces the record atomically: a half-written file would
// silently drop accounts on the next boot, which is the failure this exists to
// prevent.
func writeIdentityRecord(path, contents string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(contents), 0o600); err != nil {
		return err
	}
	if err := os.Chmod(tmp, 0o600); err != nil {
		os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// snapshotManagedIdentities records the current members of the managed group.
//
// Called after every account mutation rather than only at boot: the operator
// creates their account through the wizard while the container runs, and an
// upgrade can follow with no restart in between. Recording at boot alone would
// miss exactly the account that matters most.
//
// Best-effort by design. Failing to record must never fail the account
// operation the operator actually asked for; the appliance is merely back to the
// old behaviour of not surviving an upgrade.
func snapshotManagedIdentities(home string, members []string, shadowPath string) error {
	hashes, err := readShadowHashes(shadowPath)
	if err != nil {
		return err
	}
	ids := make([]managedIdentity, 0, len(members))
	for _, m := range members {
		ids = append(ids, managedIdentity{Username: m, Hash: hashes[m]})
	}
	return writeIdentityRecord(identityRecordPath(home), renderIdentityRecord(ids))
}
