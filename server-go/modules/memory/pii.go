package memory

// Per-attribute PII recall gating, ported from
// src/modules/memory/memory_pii_gate.c.
//
// On the pre-injection recall path this decides what reaches the context
// envelope: sensitive facts are withheld unless the turn explicitly asks for
// them, while identity facts needed for normal operation pass above a
// confidence floor. Credentials never pass at all — they are served through the
// vault, not through context.
//
// Pure like the other two stages: seed table only, no database.

// ConfidenceFloor is the minimum confidence a fact needs before it can be
// injected at all, mirroring PII_GATE_CONFIDENCE_FLOOR.
const ConfidenceFloor = 0.4

// sensitiveTurnCues are the phrases that count as the turn explicitly asking
// for a sensitive attribute. Kept specific enough to avoid incidental matches.
var sensitiveTurnCues = []string{
	"address", "phone", "email", "birthday",
	"birth date", "date of birth", "born on", "password",
	"passphrase", "credential", "secret", "api key",
	"ssn", "social security", "where do i live", "where i live",
	"my number", "home ip", "ip address",
}

// TurnRequestsSensitive reports whether the turn explicitly asks for sensitive
// information.
func TurnRequestsSensitive(turnText string) bool {
	if turnText == "" {
		return false
	}
	for _, cue := range sensitiveTurnCues {
		if ciContains(turnText, cue) {
			return true
		}
	}
	return false
}

// secretTokens name a credential outright. A relation matching one is never
// pre-injected even if nothing in the ontology knows it.
var secretTokens = []string{
	"password", "passwd", "passphrase", "secret",
	"api_key", "apikey", "access_key", "private_key",
	"privkey", "ssh_key", "token", "credential",
}

// piiTokens name a regulated identifier.
var piiTokens = []string{
	"ssn", "social_security", "passport", "credit_card", "creditcard",
	"card_number", "cvv", "bank_account", "account_number", "routing_number",
	"tax_id", "national_id", "drivers_license", "license_number", "phone",
	"email", "date_of_birth", "birthdate", "dob", "home_address",
	"street_address",
}

// unknownRelSensitivity classifies a relation the seed ontology does not know.
//
// An unknown relation defaults OPEN rather than closed: the extractor emits
// free-form relations, and withholding all of them withholds the whole layer.
// The names that plainly denote a credential or a regulated identifier are
// still gated, so being unseen is not a way past the gate.
//
// Note this is the opposite default from the DB column, where an omitted
// sensitivity reads back as PII. The two are not in conflict: the column
// describes a row someone declared and left blank, this describes a relation
// nobody declared at all.
func unknownRelSensitivity(normalized string) RelSensitivity {
	for _, token := range secretTokens {
		if ciContains(normalized, token) {
			return SensSecret
		}
	}
	for _, token := range piiTokens {
		if ciContains(normalized, token) {
			return SensPII
		}
	}
	return SensNormal
}

// RelSensitivityOf returns the sensitivity tier governing a relation: the seed
// row's when the relation is known, and the name heuristic otherwise.
//
// The heuristic is applied to the normalized name, falling back to the raw
// label when normalization empties it — a label of nothing but separators
// normalizes away, and classifying "" would silently return SensNormal.
func RelSensitivityOf(relType string) RelSensitivity {
	if relType == "" {
		return SensNormal // nothing to classify
	}
	normalized := normalizeRelType(relType)
	if normalized != "" {
		if def := seedLookup(normalized); def != nil {
			return def.Sensitivity
		}
		return unknownRelSensitivity(normalized)
	}
	return unknownRelSensitivity(relType)
}

// ShouldInject decides whether a recalled fact reaches the pre-injection
// envelope.
//
// Below the floor nothing passes, and the comparison is written so a NaN
// confidence fails closed: `confidence >= floor` is false for NaN, where
// `confidence < floor` would have been false too and let it through.
func ShouldInject(sens RelSensitivity, confidence float64, turnRequestsSensitive bool) bool {
	if !(confidence >= ConfidenceFloor) {
		return false
	}
	switch sens {
	case SensNormal:
		return true
	case SensPII:
		return turnRequestsSensitive
	}
	// SensSecret, and anything unrecognized: credentials never go through
	// pre-injection, and an unknown tier is not a reason to open the gate.
	return false
}

// ciContains is a case-insensitive substring test. An empty needle matches,
// mirroring the C.
func ciContains(haystack, needle string) bool {
	if len(needle) == 0 {
		return true
	}
	if len(needle) > len(haystack) {
		return false
	}
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if ciStartsAt(haystack, i, needle) {
			return true
		}
	}
	return false
}
