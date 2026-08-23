package db2

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"unicode"
)

// The corpus pipeline's stages, in order, from the C's k_stages.
//
// Most of them have no handler here, and that is the C's shape too: a stage
// nothing local can do is skipped rather than failed, because the pipeline is a
// ledger of where a document got to and something else may do the work.
var corpusPipelineStages = []string{
	"ingested",
	"classified",
	"sectioned",
	"chunked",
	"deduplicated",
	"references_extracted",
	"terms_normalized",
	"summarized",
	"questions_generated",
	"entities_extracted",
	"claims_extracted",
	"relationships_mapped",
	"conflicts_detected",
	"gaps_detected",
	"complete",
}

// corpusNextStage answers the stage after this one, and whether there is one.
//
// An empty stage means "ingested", which is where a document starts. A stage
// the list does not carry has no successor at all -- including "restore", which
// is reachable only by the restoration path and deliberately dead-ends here.
func corpusNextStage(stage string) (string, bool) {
	current := stage
	if current == "" {
		current = "ingested"
	}
	for index, name := range corpusPipelineStages {
		if name != current {
			continue
		}
		if index+1 >= len(corpusPipelineStages) {
			return "", false
		}
		return corpusPipelineStages[index+1], true
	}
	return "", false
}

// corpusDoc is the document a stage handler works on.
type corpusDoc struct {
	ID             int64
	Filename       string
	ContentHash    string
	NormalizedText string
}

const corpusDocQuery = `SELECT id, filename, content_hash, normalized_text
 FROM docs WHERE id = $1`

func loadCorpusDoc(ctx context.Context, tx Store, docID int64) (corpusDoc, error) {
	var doc corpusDoc
	err := tx.QueryRow(ctx, corpusDocQuery, docID).Scan(&doc.ID, &doc.Filename,
		&doc.ContentHash, &doc.NormalizedText)
	return doc, err
}

// classifyCorpusDocType is the C's rule ladder, in its order.
//
// The order is the whole classifier: a file that is code is code whatever else
// its text says, and the later rules are progressively weaker guesses with
// progressively lower confidence attached. Nothing here learns; the confidence
// is a statement about which rule fired, not about the document.
func classifyCorpusDocType(filename, text string) (string, float64) {
	lowerName := strings.ToLower(filename)
	lowerText := strings.ToLower(text)
	for _, extension := range []string{".c", ".h", ".cpp", ".cc", ".cxx",
		".hpp", ".rs", ".go", ".py", ".ts", ".js", ".java", ".cs", ".rb",
		".sh"} {
		if strings.HasSuffix(lowerName, extension) {
			return "code", 1.0
		}
	}
	for _, fence := range []string{"```c\n", "```python\n", "```rust\n",
		"```go\n"} {
		if strings.Contains(lowerText, fence) {
			return "code", 1.0
		}
	}
	switch {
	case containsAny(lowerName, "architecture", "/adr/", "adr-"),
		containsAny(lowerText, "# architecture", "architecture charter"):
		return "architecture", 0.9
	case containsAny(lowerName, "proposal", "design"),
		containsAny(lowerText, "# proposal:", "## approach"):
		return "design", 0.82
	case containsAny(lowerName, "spec", "requirements"),
		containsAny(lowerText, "acceptance criteria", "must "):
		return "spec", 0.78
	case containsAny(lowerName, "implementation", "runbook"),
		containsAny(lowerText, "rollout", "operational"):
		return "implementation", 0.72
	}
	return "other", 0.55
}

func containsAny(haystack string, needles ...string) bool {
	for _, needle := range needles {
		if strings.Contains(haystack, needle) {
			return true
		}
	}
	return false
}

const corpusClassifyUpdateQuery = `UPDATE docs
 SET doc_type = $1, doc_type_confidence = $2, updated_at = pg_now_text()
 WHERE id = $3`

// classifyCorpusDoc decides what kind of document this is, records it, and
// leaves an artifact and an audit event saying so.
//
// The artifact is committed rather than proposed because nothing reviews a
// classification: the rule that fired is the whole of the evidence, and a
// proposed artifact nobody will ever act on is a queue that only grows.
func classifyCorpusDoc(ctx context.Context, tx Store, docID int64,
	operatorID string) (string, error) {
	doc, err := loadCorpusDoc(ctx, tx, docID)
	if err != nil {
		return "", err
	}
	docType, confidence := classifyCorpusDocType(doc.Filename,
		doc.NormalizedText)
	if _, execErr := tx.Exec(ctx, corpusClassifyUpdateQuery, docType,
		confidence, docID); execErr != nil {
		return "", execErr
	}
	artifactID, idErr := newArtifactID()
	if idErr != nil {
		return "", idErr
	}
	auditID, auditIDErr := newArtifactID()
	if auditIDErr != nil {
		return "", auditIDErr
	}
	// Marshalled rather than formatted. The C builds every payload in this
	// pipeline with snprintf, and the column is JSONB: a filename with a quote
	// in it produces a payload the insert rejects.
	payload, payloadErr := json.Marshal(map[string]any{
		"doc_id":     docID,
		"filename":   doc.Filename,
		"doc_type":   docType,
		"confidence": confidence,
	})
	if payloadErr != nil {
		return "", payloadErr
	}
	if _, execErr := tx.Exec(ctx, artifactWriteQuery, artifactID,
		"doc_classification", "committed", "global", "", operatorID,
		confidence, string(payload)); execErr != nil {
		return "", execErr
	}
	after, afterErr := json.Marshal(map[string]any{
		"doc_type":            docType,
		"doc_type_confidence": confidence,
	})
	if afterErr != nil {
		return "", afterErr
	}
	if _, execErr := tx.Exec(ctx, auditEventWriteQuery, auditID, artifactID,
		"docs", fmt.Sprintf("%d", docID), operatorID, "global", "", confidence,
		false, "", string(after)); execErr != nil {
		return "", execErr
	}
	return "classified", nil
}

// corpusSection is one heading and the span of text it owns.
type corpusSection struct {
	Depth       int
	Ordinal     int64
	Heading     string
	HeadingPath string
	SpanStart   int64
	SpanEnd     int64
	ParentIndex int
}

const corpusDeleteSectionsQuery = `DELETE FROM document_sections WHERE doc_id = $1`

// The C inserts a section with a span that runs to the end of the document and
// a hash of that span, then updates both when it meets the next heading. This
// inserts each section once, with the span it actually has.
//
// The difference is not only two statements per section. The C's first hash is
// a hash of the wrong span, and it is what a reader sees for any section whose
// update never ran -- which is every section, if the rebuild failed part way.
const corpusInsertSectionQuery = `INSERT INTO document_sections
 (doc_id, parent_id, ordinal, depth, heading, heading_path, span_start,
  span_end, content_hash)
 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9) RETURNING id`

// rebuildCorpusSections replaces a document's section tree from its headings.
//
// The whole thing runs inside the drain's transaction, which the C's does not:
// the C deletes the sections and then rebuilds them with no boundary between,
// so a failure part way leaves a document with fewer sections than it has
// headings and nothing saying so.
func rebuildCorpusSections(ctx context.Context, tx Store, docID int64) (
	string, error,
) {
	doc, err := loadCorpusDoc(ctx, tx, docID)
	if err != nil {
		return "", err
	}
	if _, execErr := tx.Exec(ctx, corpusDeleteSectionsQuery,
		docID); execErr != nil {
		return "", execErr
	}
	sections := parseCorpusSections(doc.NormalizedText)
	identifiers := make([]int64, len(sections))
	for index, section := range sections {
		var parentID *int64
		if section.ParentIndex >= 0 {
			parentID = &identifiers[section.ParentIndex]
		}
		hash := fnv1a64Hex(doc.NormalizedText[section.SpanStart:section.SpanEnd])
		if scanErr := tx.QueryRow(ctx, corpusInsertSectionQuery, docID,
			parentID, section.Ordinal, section.Depth, section.Heading,
			section.HeadingPath, section.SpanStart, section.SpanEnd, hash).
			Scan(&identifiers[index]); scanErr != nil {
			// A heading path repeated at the same ordinal still collides with
			// the table's uniqueness rule -- counting siblings makes that far
			// rarer, not impossible. The insert is left failing rather than
			// merged: merging two sections into one row would lose a span
			// nothing could recover, and the drain records the failure on the
			// job where somebody can see it.
			return "", scanErr
		}
	}
	return fmt.Sprintf("sections=%d", len(sections)), nil
}

// parseCorpusSections walks the document once and answers its sections with
// the spans they actually own.
//
// A heading closes every open section at its depth or deeper, which is what
// makes the spans nest: a section ends where the next heading that is not
// inside it begins.
func parseCorpusSections(text string) []corpusSection {
	sections := []corpusSection{}
	// open[d] is the index of the section currently open at depth d, or -1.
	open := [7]int{-1, -1, -1, -1, -1, -1, -1}
	ordinals := [7]int64{}
	headings := [7]string{}
	offset := 0
	for offset < len(text) {
		lineEnd := strings.IndexByte(text[offset:], '\n')
		lineLength := len(text) - offset
		if lineEnd >= 0 {
			lineLength = lineEnd + 1
		}
		line := text[offset : offset+lineLength]
		depth, heading, isHeading := parseCorpusHeading(line)
		if isHeading {
			for closing := depth; closing <= 6; closing++ {
				if open[closing] >= 0 {
					sections[open[closing]].SpanEnd = int64(offset)
				}
				open[closing] = -1
				headings[closing] = ""
				// The ordinal at this heading's own depth is deliberately
				// kept. The C clears it and then increments it, so every
				// section it has ever written has ordinal one -- which makes
				// the column say nothing and makes the table's own uniqueness
				// rule, on (doc, heading path, ordinal), collide whenever a
				// document repeats a heading. Counting siblings is what the
				// column means and what stops the collision.
				if closing > depth {
					ordinals[closing] = 0
				}
			}
			headings[depth] = heading
			parentIndex := -1
			for above := depth - 1; above >= 1; above-- {
				if open[above] >= 0 {
					parentIndex = open[above]
					break
				}
			}
			ordinals[depth]++
			sections = append(sections, corpusSection{
				Depth:       depth,
				Ordinal:     ordinals[depth],
				Heading:     heading,
				HeadingPath: corpusHeadingPath(headings, depth),
				SpanStart:   int64(offset),
				SpanEnd:     int64(len(text)),
				ParentIndex: parentIndex,
			})
			open[depth] = len(sections) - 1
		}
		offset += lineLength
	}
	return sections
}

// parseCorpusHeading answers the depth and text of an ATX heading.
//
// A run of hashes with no space after it is not a heading, and neither is one
// with nothing after the space: both are things a document says, not sections
// it has.
func parseCorpusHeading(line string) (int, string, bool) {
	depth := 0
	for depth < 6 && depth < len(line) && line[depth] == '#' {
		depth++
	}
	if depth == 0 || depth >= len(line) || line[depth] != ' ' {
		return 0, "", false
	}
	heading := strings.TrimLeft(line[depth+1:], " \t")
	heading = strings.TrimRightFunc(heading, unicode.IsSpace)
	if heading == "" {
		return 0, "", false
	}
	if len(heading) >= corpusSectionHeadingMax {
		heading = heading[:corpusSectionHeadingMax-1]
	}
	return depth, heading, true
}

// The C's CORPUS_SECTION_HEADING and CORPUS_SECTION_PATH: the widths a heading
// and a heading path are stored in.
const (
	corpusSectionHeadingMax = 256
	corpusSectionPathMax    = 1024
)

// corpusHeadingPath joins the open headings above this one, the C's separator
// included.
func corpusHeadingPath(headings [7]string, depth int) string {
	parts := []string{}
	for level := 1; level <= depth; level++ {
		if headings[level] == "" {
			continue
		}
		parts = append(parts, headings[level])
	}
	path := strings.Join(parts, " > ")
	if len(path) >= corpusSectionPathMax {
		path = path[:corpusSectionPathMax-1]
	}
	return path
}
