package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageRulesList,
		db2contract.OperationRulesList, rulesList)
	Register(db2contract.StageRulesListHard,
		db2contract.OperationRulesListHard, rulesListHard)
	Register(db2contract.StageRulesListByTier,
		db2contract.OperationRulesListByTier, rulesListByTier)
	Register(db2contract.StageRulesFindByTitle,
		db2contract.OperationRulesFindByTitle, rulesFindByTitle)
	Register(db2contract.StageTaskGet,
		db2contract.OperationTaskGet, taskGet)
	Register(db2contract.StageTaskSubtasks,
		db2contract.OperationTaskSubtasks, taskSubtasks)
}

// The ten columns every rules read returns, including expires_at.
//
// The C's shared column list has nine and leaves expires_at out, while the
// struct it fills carries the field and the reply declares it -- so every rules
// reply has been answering an empty expiry for a rule that has one. A reader
// treating empty as "never expires" therefore keeps applying a rule past its
// expiry, which is the failure the column exists to prevent. Selecting it is
// the fix; it is called out here because the reply changes shape in practice
// even though it does not change on the wire.
//
// COALESCE because the column is nullable and most rules have no expiry: absent
// reads as empty, which is what the reply's field means.
const rulesColumns = `SELECT id, weight, polarity, title, description, domain,
 created_at, updated_at, directive_type, COALESCE(expires_at, '')
 FROM rules`

// Heaviest first, then alphabetical. Weight is what decides whether a rule is a
// rule or an inclination, so a reader taking the top of the list takes the
// binding ones -- and the title breaks ties so the same set renders in the same
// order twice running.
const rulesOrder = ` ORDER BY weight DESC, title ASC LIMIT $1`

const (
	rulesListQuery       = rulesColumns + rulesOrder
	rulesListHardQuery   = rulesColumns + ` WHERE directive_type = 'hard'` + rulesOrder
	rulesListByTierQuery = rulesColumns + ` WHERE weight >= $2` + rulesOrder
)

// readRules collects the shared ten-column shape.
func readRules(ctx context.Context, store Store, query string, args []any,
	ceiling int,
) ([]db2contract.RulesListRow, error) {
	rows, err := store.Query(ctx, query, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	found := make([]db2contract.RulesListRow, 0, 16)
	for rows.Next() && len(found) < ceiling {
		var id, weight int64
		var polarity, title, description, domain string
		var createdAt, updatedAt, directiveType, expiresAt string
		if scanErr := rows.Scan(&id, &weight, &polarity, &title, &description,
			&domain, &createdAt, &updatedAt, &directiveType,
			&expiresAt); scanErr != nil {
			return nil, scanErr
		}
		found = append(found, db2contract.RulesListRow{
			RuleID:            clampToU32(id),
			RuleWeight:        clampToU32(weight),
			Polarity:          polarity,
			RuleTitle:         title,
			RuleDescription:   description,
			Domain:            domain,
			RuleCreatedAt:     createdAt,
			RuleUpdatedAt:     updatedAt,
			RuleDirectiveType: directiveType,
			ExpiresAt:         expiresAt,
		})
	}
	return found, rows.Err()
}

// rulesList lists every rule.
func rulesList(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeRulesListRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.RulesListMaxRows
	found, queryErr := readRules(ctx, store, rulesListQuery,
		[]any{int64(ceiling)}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeRulesListReply(found)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// rulesListHard lists the rules that are not negotiable.
//
// A hard directive is one a caller may not weigh against anything else, which
// is a different question from how heavy it is -- a soft rule of weight ninety
// still yields to judgement and a hard rule of weight ten does not.
func rulesListHard(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if err := db2contract.DecodeRulesListHardRequest(request); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.RulesListHardMaxRows
	rules, queryErr := readRules(ctx, store, rulesListHardQuery,
		[]any{int64(ceiling)}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	hard := make([]db2contract.RulesListHardRow, len(rules))
	for index, rule := range rules {
		hard[index] = db2contract.RulesListHardRow(rule)
	}
	reply, encodeErr := db2contract.EncodeRulesListHardReply(hard)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// rulesListByTier lists the rules at or above a weight.
//
// The tiers are named elsewhere -- seventy-five and up is a rule, fifty and up
// an inclination -- and this takes the boundary rather than the name, so a
// caller can ask for a cut the naming does not have a word for.
func rulesListByTier(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	minWeight, err := db2contract.DecodeRulesListByTierRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.RulesListByTierMaxRows
	rules, queryErr := readRules(ctx, store, rulesListByTierQuery,
		[]any{int64(ceiling), int64(minWeight)}, ceiling)
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	tier := make([]db2contract.RulesListByTierRow, len(rules))
	for index, rule := range rules {
		tier[index] = db2contract.RulesListByTierRow(rule)
	}
	reply, encodeErr := db2contract.EncodeRulesListByTierReply(tier)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Case-insensitive, and bounded to one row.
//
// Nothing constrains rules to unique titles, so this can match several; the C
// takes the first the planner gives, and LIMIT makes that explicit rather than
// incidental. The feedback write ported earlier reinforces whichever this
// finds, so the two agree on which rule a title means.
const rulesFindByTitleQuery = `SELECT id, polarity, description, weight, domain,
 directive_type, COALESCE(expires_at, '')
 FROM rules WHERE LOWER(title) = LOWER($1) LIMIT 1`

// rulesFindByTitle answers the rule filed under a title.
func rulesFindByTitle(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	title, err := db2contract.DecodeRulesFindByTitleRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id, weight int64
	var polarity, description, domain, directiveType, expiresAt string
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, rulesFindByTitleQuery, title).Scan(&id,
		&polarity, &description, &weight, &domain, &directiveType,
		&expiresAt); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, id, weight = 0, 0, 0
		polarity, description, domain, directiveType, expiresAt = "", "", "", "", ""
	}
	reply, encodeErr := db2contract.EncodeRulesFindByTitleReply(found,
		clampToU32(id), polarity, description, clampToU32(weight), domain,
		directiveType, expiresAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The eight columns both task reads return. parent_id and session_id are the
// nullable pair: a root task has no parent, and a task can be created outside
// a session.
const taskColumns = `SELECT id, COALESCE(parent_id, 0), title, state, confidence,
 COALESCE(session_id, ''), created_at, updated_at
 FROM tasks`

const (
	taskGetQuery      = taskColumns + ` WHERE id = $1`
	taskSubtasksQuery = taskColumns + ` WHERE parent_id = $1 ORDER BY created_at ASC LIMIT $2`
)

// taskGet answers what a task is and where it stands.
func taskGet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	taskID, err := db2contract.DecodeTaskGetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var id, parentID int64
	var title, state, sessionID, createdAt, updatedAt string
	var confidence float64
	found := uint32(1)
	if scanErr := store.QueryRow(ctx, taskGetQuery, int64(taskID)).Scan(&id,
		&parentID, &title, &state, &confidence, &sessionID, &createdAt,
		&updatedAt); scanErr != nil {
		if !rowAbsent(scanErr) {
			return nil, bus.ModuleStatusInternal
		}
		found, parentID, confidence = 0, 0, 0
		title, state, sessionID, createdAt, updatedAt = "", "", "", "", ""
	}
	reply, encodeErr := db2contract.EncodeTaskGetReply(found,
		clampToU64(parentID), title, state, confidence, sessionID, createdAt,
		updatedAt)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// taskSubtasks lists what a task was broken into.
//
// Oldest first, by creation, because a decomposition is read as a plan: the
// order the pieces were written down is the order someone intended to do them
// in, and nothing else records that.
func taskSubtasks(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	parentTask, err := db2contract.DecodeTaskSubtasksRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ceiling := db2contract.TaskSubtasksMaxRows
	rows, queryErr := store.Query(ctx, taskSubtasksQuery,
		int64(parentTask), int64(ceiling))
	if queryErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()

	subtasks := make([]db2contract.TaskSubtasksRow, 0, 8)
	for rows.Next() {
		var id, parentID int64
		var title, state, sessionID, createdAt, updatedAt string
		var confidence float64
		if scanErr := rows.Scan(&id, &parentID, &title, &state, &confidence,
			&sessionID, &createdAt, &updatedAt); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		subtasks = append(subtasks, db2contract.TaskSubtasksRow{
			TaskRowID:      clampToU64(id),
			ParentTaskID:   clampToU64(parentID),
			TaskConfidence: confidence,
			TaskTitle:      title,
			TaskState:      state,
			TaskCreatedAt:  createdAt,
			TaskUpdatedAt:  updatedAt,
			TaskSessionID:  sessionID,
		})
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeTaskSubtasksReply(subtasks)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
