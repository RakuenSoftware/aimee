package memory

import (
	"strings"
	"unicode"
	"unicode/utf8"

	"golang.org/x/text/cases"
	"golang.org/x/text/unicode/norm"
)

// Existing fact identities use NFKC, full case folding, collapsed Unicode
// whitespace and unit separators. Never truncate an identity into a collision.
func factIdentityComponent(value string) string {
	if !utf8.ValidString(value) || strings.ContainsRune(value, 0) {
		return ""
	}
	value = cases.Fold().String(norm.NFKC.String(value))
	value = strings.Join(strings.FieldsFunc(value, func(r rune) bool { return unicode.IsSpace(r) || r >= 0x1c && r <= 0x1f }), " ")
	if len(value) >= 1024 {
		return ""
	}
	return value
}

func factIdentity(source, relation, target string) (identity, subject string) {
	source, relation, target = factIdentityComponent(source), normalizeRelType(relation), factIdentityComponent(target)
	if source == "" || relation == "" {
		return "", ""
	}
	subject = source + "\x1f" + relation
	if len(subject) >= 1024 {
		return "", ""
	}
	if target != "" {
		identity = subject + "\x1f" + target
	}
	if len(identity) >= 1024 {
		identity = ""
	}
	return
}

var functionalFactRelations = []string{"lives_in", "born_in", "age", "located_in", "has_hostname", "spouse", "works_for", "has_role", "device_has_ip"}

func factFunctional(relation string) bool {
	for _, name := range functionalFactRelations {
		if relation == name {
			return true
		}
	}
	return false
}

// Family and birthplace identities refuse inferred retractions. An explicit
// verified user correction remains authoritative, matching the seed ontology.
func factImmutable(relation string) bool {
	switch relation {
	case "parent_of", "child_of", "born_in":
		return true
	}
	return false
}
