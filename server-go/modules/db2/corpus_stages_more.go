package db2

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"unicode"
)

// corpusReference is one reference a document makes.
type corpusReference struct {
	Type       string
	Target     string
	Offset     int64
	Confidence float64
}

const corpusDeleteReferencesQuery = `DELETE FROM document_references
 WHERE from_doc_id = $1`

// The C runs three statements per reference: find the section the offset falls
// in, resolve the target to a document, then insert. This runs one for the
// whole document.
//
// The section and the target are subqueries because both are lookups against
// rows this statement already needs, and doing them separately is what let the
// C see a section that a concurrent rebuild had already replaced.
//
// A target with a scheme is external and is not resolved at all: a URL that
// happens to share a filename with a document in the corpus is not that
// document.
const corpusInsertReferencesQuery = `INSERT INTO document_references
 (from_doc_id, from_section_id, ref_type, raw_target, to_doc_id, resolution,
  confidence)
 SELECT $1,
   (SELECT s.id FROM document_sections s
     WHERE s.doc_id = $1 AND s.span_start <= reference.at_offset
       AND s.span_end >= reference.at_offset
     ORDER BY s.depth DESC, s.span_start DESC LIMIT 1),
   reference.ref_type, reference.raw_target, resolved.id,
   CASE WHEN reference.external THEN 'external'
        WHEN resolved.id IS NOT NULL THEN 'resolved'
        ELSE 'unresolved' END,
   reference.confidence
  FROM unnest($2::text[], $3::text[], $4::bigint[], $5::double precision[],
              $6::boolean[], $7::text[])
    AS reference(ref_type, raw_target, at_offset, confidence, external, like_target)
  CROSS JOIN LATERAL (
    SELECT (SELECT d.id FROM docs d
             WHERE NOT reference.external AND d.id <> $1
               AND (d.filename = reference.raw_target
                    OR d.filename LIKE reference.like_target
                    OR d.content_hash = reference.raw_target)
             ORDER BY d.id ASC LIMIT 1)) AS resolved(id)
 ON CONFLICT (from_doc_id, ref_type, raw_target) DO UPDATE SET
   from_section_id = EXCLUDED.from_section_id,
   to_doc_id = EXCLUDED.to_doc_id,
   resolution = EXCLUDED.resolution,
   confidence = EXCLUDED.confidence`

// extractCorpusReferences rebuilds what a document points at.
//
// Two patterns, both the C's: a markdown link target, and a bare word after
// "see" that looks like a filename. The second is deliberately weak -- it is
// recorded at a lower confidence and exists to catch prose that names a
// document without linking it.
func extractCorpusReferences(ctx context.Context, tx Store, docID int64) (
	string, error,
) {
	doc, err := loadCorpusDoc(ctx, tx, docID)
	if err != nil {
		return "", err
	}
	if _, execErr := tx.Exec(ctx, corpusDeleteReferencesQuery,
		docID); execErr != nil {
		return "", execErr
	}
	references := scanCorpusReferences(doc.NormalizedText)
	if len(references) == 0 {
		return "references=0", nil
	}
	types := make([]string, len(references))
	targets := make([]string, len(references))
	offsets := make([]int64, len(references))
	confidences := make([]float64, len(references))
	external := make([]bool, len(references))
	likeTargets := make([]string, len(references))
	for index, reference := range references {
		types[index] = reference.Type
		targets[index] = reference.Target
		offsets[index] = reference.Offset
		confidences[index] = reference.Confidence
		external[index] = strings.Contains(reference.Target, "://")
		base := reference.Target
		if cut := strings.LastIndexByte(base, '/'); cut >= 0 {
			base = base[cut+1:]
		}
		likeTargets[index] = "%" + base
	}
	if _, execErr := tx.Exec(ctx, corpusInsertReferencesQuery, docID, types,
		targets, offsets, confidences, external, likeTargets); execErr != nil {
		return "", execErr
	}
	return fmt.Sprintf("references=%d", len(references)), nil
}

// scanCorpusReferences finds both patterns and drops the duplicates.
//
// The C does not deduplicate, and its count is of insert attempts rather than
// references: a document that links the same target twice claims two. The last
// spelling of a duplicate wins, which is the C's own outcome -- its second
// insert updates the row the first wrote.
func scanCorpusReferences(text string) []corpusReference {
	found := []corpusReference{}
	position := map[string]int{}
	add := func(reference corpusReference) {
		reference.Target = cleanCorpusRefTarget(reference.Target)
		if reference.Target == "" {
			return
		}
		key := reference.Type + "\x1f" + reference.Target
		if index, seen := position[key]; seen {
			found[index] = reference
			return
		}
		position[key] = len(found)
		found = append(found, reference)
	}
	for offset := 0; ; {
		relative := strings.Index(text[offset:], "](")
		if relative < 0 {
			break
		}
		start := offset + relative + 2
		end := strings.IndexByte(text[start:], ')')
		if end > 0 {
			add(corpusReference{
				Type: "cites", Target: text[start : start+end],
				Offset: int64(start), Confidence: 1.0,
			})
		}
		offset = start
	}
	lowered := strings.ToLower(text)
	for offset := 0; ; {
		relative := strings.Index(lowered[offset:], "see ")
		if relative < 0 {
			break
		}
		start := offset + relative + 4
		for start < len(text) && (text[start] == ' ' || text[start] == '\t') {
			start++
		}
		end := start
		for end < len(text) && !unicode.IsSpace(rune(text[end])) {
			end++
		}
		// A word with no dot in it is not a filename, and a reference to
		// something that is not a document is noise the corpus cannot resolve.
		if word := text[start:end]; strings.Contains(word, ".") {
			add(corpusReference{
				Type: "mentions", Target: word,
				Offset: int64(start), Confidence: 0.75,
			})
		}
		offset = offset + relative + 4
	}
	return found
}

// cleanCorpusRefTarget strips the punctuation a target picks up from the prose
// around it -- a closing bracket, a comma, a full stop.
func cleanCorpusRefTarget(target string) string {
	if len(target) >= corpusRefTargetMax {
		target = target[:corpusRefTargetMax-1]
	}
	return strings.TrimRightFunc(target, func(character rune) bool {
		return character == ')' || character == ',' || character == '.' ||
			character == ';' || unicode.IsSpace(character)
	})
}

// corpusRefTargetMax is the C's CORPUS_REF_TARGET_LEN.
const corpusRefTargetMax = 512

// The candidate terms that no live mapping already covers.
//
// The C asks that question once per candidate, so fifty candidates are fifty
// ILIKE scans of the artifact payloads. This asks it once for all of them.
const corpusKnownTermsQuery = `SELECT candidate.term
 FROM unnest($1::text[]) AS candidate(term)
 WHERE EXISTS (SELECT 1 FROM artifacts
   WHERE kind = 'term_mapping' AND state <> 'retired'
     AND payload::text ILIKE '%' || candidate.term || '%')`

const corpusWriteTermMappingsQuery = `INSERT INTO artifacts
 (id, kind, state, scope_kind, scope_id, operator_id, confidence,
  attempt_count, source_bundle_hash, model_version, prompt_version,
  target_surface, created_at, payload)
 SELECT mapping.id, 'term_mapping', 'proposed', 'global', 'global',
        'corpus.terms', 0.7, 1, '', '', '', '', pg_now_text(),
        mapping.payload::jsonb
   FROM unnest($1::text[], $2::text[]) AS mapping(id, payload)
 ON CONFLICT (id) DO NOTHING`

const corpusCiteTermMappingsQuery = `INSERT INTO artifact_citations
 (artifact_id, source_kind, source_id, span_start, span_end)
 SELECT citation.id, 'doc', $2, 0, 0
   FROM unnest($1::text[]) AS citation(id)
 ON CONFLICT DO NOTHING`

// normalizeCorpusTerms proposes a canonical spelling for the surface terms a
// document uses.
//
// Proposed, not committed: a canonicalisation is a claim about what two
// spellings mean, and nothing here can tell an alias from a distinct term.
func normalizeCorpusTerms(ctx context.Context, tx Store, docID int64) (
	string, error,
) {
	doc, err := loadCorpusDoc(ctx, tx, docID)
	if err != nil {
		return "", err
	}
	if doc.NormalizedText == "" {
		return "terms=0", nil
	}
	candidates := collectCorpusTerms(doc.NormalizedText)
	if len(candidates) == 0 {
		return "terms=0", nil
	}
	known, knownErr := readKnownCorpusTerms(ctx, tx, candidates)
	if knownErr != nil {
		return "", knownErr
	}
	identifiers := []string{}
	payloads := []string{}
	for _, candidate := range candidates {
		if known[candidate] {
			continue
		}
		artifactID, idErr := newArtifactID()
		if idErr != nil {
			return "", idErr
		}
		// Marshalled rather than formatted: a term is lifted verbatim out of a
		// quoted span, so a term containing a quote is not unusual, and the C's
		// snprintf-ed payload would not be JSON.
		payload, payloadErr := json.Marshal(map[string]string{
			"raw_term":       candidate,
			"preferred_term": canonicalCorpusTerm(candidate),
			"term_kind":      "alias",
			"scope_note":     "auto-detected",
		})
		if payloadErr != nil {
			return "", payloadErr
		}
		identifiers = append(identifiers, artifactID)
		payloads = append(payloads, string(payload))
	}
	if len(identifiers) == 0 {
		return "terms=0", nil
	}
	if _, execErr := tx.Exec(ctx, corpusWriteTermMappingsQuery, identifiers,
		payloads); execErr != nil {
		return "", execErr
	}
	// The citation is what makes the proposal reviewable: it says which
	// document the term was read in.
	if _, execErr := tx.Exec(ctx, corpusCiteTermMappingsQuery, identifiers,
		fmt.Sprintf("%d", docID)); execErr != nil {
		return "", execErr
	}
	return fmt.Sprintf("terms=%d", len(identifiers)), nil
}

func readKnownCorpusTerms(ctx context.Context, tx Store, candidates []string) (
	map[string]bool, error,
) {
	rows, err := tx.Query(ctx, corpusKnownTermsQuery, candidates)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	known := map[string]bool{}
	for rows.Next() {
		var term string
		if scanErr := rows.Scan(&term); scanErr != nil {
			return nil, scanErr
		}
		known[term] = true
	}
	return known, rows.Err()
}

// canonicalCorpusTerm is the C's canonical_form: lowercased, with the
// whitespace removed rather than collapsed.
func canonicalCorpusTerm(term string) string {
	var canonical strings.Builder
	for _, character := range term {
		if unicode.IsSpace(character) {
			continue
		}
		canonical.WriteRune(unicode.ToLower(character))
	}
	return canonical.String()
}

// corpusTermCap and corpusTermLengths are the C's MAX_TOK and tok_len_ok: at
// most fifty candidates, each between two and sixty characters.
const corpusTermCap = 50

// collectCorpusTerms is the C's tok_collect.
//
// Three things make a candidate: a quoted or backticked span, a run of word
// characters, and a CamelCase boundary, which ends the current run rather than
// extending it -- so "parseHeadingPath" yields "parse", "Heading" and "Path"
// separately, and none of them is the whole identifier. That is the C's
// behaviour and it is what the existing term mappings were derived from.
func collectCorpusTerms(text string) []string {
	terms := []string{}
	var current strings.Builder
	inWord := false
	flush := func() {
		if inWord && corpusTermLengthOK(current.Len()) &&
			len(terms) < corpusTermCap {
			terms = append(terms, current.String())
		}
		current.Reset()
		inWord = false
	}
	for index := 0; index < len(text) && len(terms) < corpusTermCap; index++ {
		character := text[index]
		if character == '`' || character == '"' || character == '\'' {
			current.Reset()
			inWord = false
			index++
			start := index
			for index < len(text) && text[index] != character {
				index++
			}
			quoted := text[start:index]
			if len(quoted) >= corpusTermBufferMax {
				quoted = quoted[:corpusTermBufferMax-1]
			}
			if corpusTermLengthOK(len(quoted)) && len(terms) < corpusTermCap {
				terms = append(terms, quoted)
			}
			continue
		}
		if isCorpusTermSpaceOrPunct(character) {
			flush()
			continue
		}
		lower := character >= 'a' && character <= 'z'
		digit := character >= '0' && character <= '9'
		upper := character >= 'A' && character <= 'Z'
		switch {
		case lower || digit:
			// A lowercase byte cannot start a word: the C only opens one on an
			// uppercase byte, which is what makes these candidates names
			// rather than every word in the document.
			if inWord && current.Len() < corpusTermBufferMax-1 {
				current.WriteByte(character)
			}
		case upper:
			if inWord {
				previous := current.String()
				if last := previous[len(previous)-1]; (last >= 'a' &&
					last <= 'z') || (last >= '0' && last <= '9') {
					flush()
				}
			}
			if !inWord {
				current.Reset()
				inWord = true
			}
			if current.Len() < corpusTermBufferMax-1 {
				current.WriteByte(character)
			}
		case character == '_':
			if inWord && current.Len() < corpusTermBufferMax-1 {
				current.WriteByte(character)
			}
		}
	}
	flush()
	return terms
}

// corpusTermBufferMax is the C's token buffer width.
const corpusTermBufferMax = 64

func corpusTermLengthOK(length int) bool {
	return length >= 2 && length < 60
}

// isCorpusTermSpaceOrPunct is C's isspace() || ispunct() over a byte, minus the
// characters the tokenizer handles itself.
func isCorpusTermSpaceOrPunct(character byte) bool {
	if character == '_' {
		return false
	}
	if character >= 0x80 {
		return false
	}
	return unicode.IsSpace(rune(character)) || unicode.IsPunct(rune(character)) ||
		unicode.IsSymbol(rune(character))
}
