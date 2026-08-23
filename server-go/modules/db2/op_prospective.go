package db2

import (
	"context"
	"strings"
	"unicode"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageProspectiveList,
		db2contract.OperationProspectiveList, prospectiveList)
	Register(db2contract.StageProspectiveListArmed,
		db2contract.OperationProspectiveListArmed, prospectiveListArmed)
	Register(db2contract.StageProspectiveByEntity,
		db2contract.OperationProspectiveByEntity, prospectiveByEntity)
	Register(db2contract.StageProspectiveByFile,
		db2contract.OperationProspectiveByFile, prospectiveByFile)
	Register(db2contract.StageProspectiveByTriggerTerms,
		db2contract.OperationProspectiveByTriggerTerms, prospectiveByTriggerTerms)
}

// The thirteen columns every prospective read returns, and the ordering they
// all share: newest first, with the identifier breaking ties so two reminders
// created in the same second come back in a stable order.
const (
	prospectiveColumns = `SELECT id, trigger_text, action_text, anchor_entity,
 anchor_file, recurrence, state, valid_until, source_session, trigger_count,
 last_triggered_at, created_at, updated_at
 FROM prospective_memories`
	prospectiveOrder = ` ORDER BY created_at DESC, id DESC LIMIT `
)

// Four of the five reads want only armed reminders. A reminder that has fired
// or been cancelled is not something to fire again, and the three trigger
// reads exist to answer "what should fire now".
const (
	prospectiveListQuery = prospectiveColumns +
		` WHERE ($2 = '' OR state = $2)` + prospectiveOrder + `$1`
	prospectiveArmedQuery = prospectiveColumns +
		` WHERE state = 'armed'` + prospectiveOrder + `$1`
	prospectiveByEntityQuery = prospectiveColumns +
		` WHERE state = 'armed' AND LOWER(anchor_entity) = $2` +
		prospectiveOrder + `$1`
	prospectiveByFileQuery = prospectiveColumns +
		` WHERE state = 'armed' AND anchor_file = $2` + prospectiveOrder + `$1`
	// The stored tsvector column is built with the 'simple' configuration; this
	// recomputes with 'english' so a prefix query picks up morphology -- asking
	// about "rotate" finds a reminder about rotation. The C's note says the
	// recomputation is affordable because the table is small, and it is the
	// difference between a reminder firing and not.
	prospectiveByTriggerQuery = prospectiveColumns +
		` WHERE state = 'armed'
   AND to_tsvector('english', trigger_text) @@ to_tsquery('english', $2)` +
		prospectiveOrder + `$1`
)

// The words a trigger search ignores. Broad chatter should not fire every armed
// reminder, and these are the words most likely to appear in any turn.
var prospectiveStopWords = map[string]bool{
	"the": true, "a": true, "an": true, "and": true, "or": true, "of": true,
	"to": true, "is": true, "are": true, "was": true, "has": true,
	"have": true, "that": true, "this": true, "it": true, "its": true,
	"be": true, "for": true, "on": true, "in": true,
}

// prospectiveMinimumTokenLength is the shortest word worth searching on. Two
// letters carry too little to distinguish one reminder from another.
const prospectiveMinimumTokenLength = 3

// buildTriggerQuery turns a turn of conversation into a tsquery.
//
// Words are lowercased, joined with OR, and each is a prefix match -- so
// "rotating" in the turn finds a reminder about "rotate". Short and common
// words are dropped, and if nothing survives there is no query: an empty one
// would either fail to parse or match everything, and both are worse than
// answering that nothing triggers.
func buildTriggerQuery(turnText string) string {
	var terms []string
	seen := map[string]bool{}
	for _, field := range strings.FieldsFunc(turnText, func(r rune) bool {
		return !unicode.IsLetter(r) && !unicode.IsDigit(r) && r != '_'
	}) {
		token := strings.ToLower(field)
		if len(token) < prospectiveMinimumTokenLength ||
			prospectiveStopWords[token] || seen[token] {
			continue
		}
		seen[token] = true
		terms = append(terms, token+":*")
	}
	return strings.Join(terms, " | ")
}

// readProspective collects the shared thirteen-column shape.
func readProspective(ctx context.Context, store Store, query string, args []any,
	ceiling int,
) ([]db2contract.ProspectiveListRow, error) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	found := make([]db2contract.ProspectiveListRow, 0, 16)
	for rows.Next() && len(found) < ceiling {
		var id, triggerCount int64
		var trigger, action, entity, file, recurrence, state string
		var validUntil, session, lastTriggered, createdAt, updatedAt string
		if scanErr := rows.Scan(&id, &trigger, &action, &entity, &file,
			&recurrence, &state, &validUntil, &session, &triggerCount,
			&lastTriggered, &createdAt, &updatedAt); scanErr != nil {
			return nil, scanErr
		}
		found = append(found, db2contract.ProspectiveListRow{
			ProspectiveID:   clampToU64(id),
			TriggerText:     trigger,
			ActionText:      action,
			AnchorEntity:    entity,
			AnchorFile:      file,
			Recurrence:      recurrence,
			State:           state,
			ValidUntil:      validUntil,
			SourceSession:   session,
			TriggerCount:    clampToU32(triggerCount),
			LastTriggeredAt: lastTriggered,
			CreatedAt:       createdAt,
			UpdatedAt:       updatedAt,
		})
	}
	return found, rows.Err()
}

// prospectiveList lists reminders, optionally in one state.
//
// An empty state means every state, which is the C's two statements said once.
// This is the only one of the five that will show a fired or cancelled
// reminder -- it is the maintenance view rather than a trigger.
func prospectiveList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	stateFilter, limit, err := db2contract.DecodeProspectiveListRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProspectiveListMaxRows
	found, queryErr := readProspective(ctx, store, prospectiveListQuery,
		[]any{int64(pairLimit(limit, ceiling)), stateFilter}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeProspectiveListReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// prospectiveListArmed lists every reminder waiting to fire.
func prospectiveListArmed(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeProspectiveListArmedRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProspectiveListArmedMaxRows
	found, queryErr := readProspective(ctx, store, prospectiveArmedQuery,
		[]any{int64(ceiling)}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	armed := make([]db2contract.ProspectiveListArmedRow, len(found))
	for index, row := range found {
		armed[index] = db2contract.ProspectiveListArmedRow(row)
	}
	reply, encodeErr := db2contract.EncodeProspectiveListArmedReply(armed)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// prospectiveByEntity lists armed reminders anchored to an entity.
//
// The stored anchor is lowered for the comparison rather than the parameter,
// because the caller is expected to have lowered its side already -- the field
// is named for it. Lowering both would be safer and is what the C does not do.
func prospectiveByEntity(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	entity, limit, err := db2contract.DecodeProspectiveByEntityRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProspectiveByEntityMaxRows
	found, queryErr := readProspective(ctx, store, prospectiveByEntityQuery,
		[]any{int64(pairLimit(limit, ceiling)), entity}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	byEntity := make([]db2contract.ProspectiveByEntityRow, len(found))
	for index, row := range found {
		byEntity[index] = db2contract.ProspectiveByEntityRow(row)
	}
	reply, encodeErr := db2contract.EncodeProspectiveByEntityReply(byEntity)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// prospectiveByFile lists armed reminders anchored to a file.
//
// Exact path match, unlike the entity read's lowering: a path is a path, and
// two files differing only in case are two files on the systems this runs on.
func prospectiveByFile(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	filePath, limit, err := db2contract.DecodeProspectiveByFileRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProspectiveByFileMaxRows
	found, queryErr := readProspective(ctx, store, prospectiveByFileQuery,
		[]any{int64(pairLimit(limit, ceiling)), filePath}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	byFile := make([]db2contract.ProspectiveByFileRow, len(found))
	for index, row := range found {
		byFile[index] = db2contract.ProspectiveByFileRow(row)
	}
	reply, encodeErr := db2contract.EncodeProspectiveByFileReply(byFile)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// prospectiveByTriggerTerms lists armed reminders a turn of conversation might
// be about.
//
// A turn with nothing searchable in it answers nothing rather than running a
// query: an empty tsquery either fails to parse or matches everything, and a
// reminder firing on "it is the a" would be worse than one not firing at all.
func prospectiveByTriggerTerms(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	turnText, limit, err :=
		db2contract.DecodeProspectiveByTriggerTermsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.ProspectiveByTriggerTermsMaxRows

	found := []db2contract.ProspectiveListRow{}
	if query := buildTriggerQuery(turnText); query != "" {
		var queryErr error
		found, queryErr = readProspective(ctx, store, prospectiveByTriggerQuery,
			[]any{int64(pairLimit(limit, ceiling)), query}, ceiling)
		if queryErr != nil {
			return nil, bus.ModuleStatusInternal
		}
	}
	byTrigger := make([]db2contract.ProspectiveByTriggerTermsRow, len(found))
	for index, row := range found {
		byTrigger[index] = db2contract.ProspectiveByTriggerTermsRow(row)
	}
	reply, encodeErr :=
		db2contract.EncodeProspectiveByTriggerTermsReply(byTrigger)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
