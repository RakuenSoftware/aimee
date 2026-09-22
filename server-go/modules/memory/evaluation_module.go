package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// EvaluationModule is the production module with caller-supplied test storage
// and dependencies. The caller owns the store lifetime. It never retargets a
// live module or registers a second implementation on the bus.
type EvaluationModule struct {
	ctx     context.Context
	backend *postgresDataStore
	db      store.DB
	Handler bus.ModuleHandler
}

func NewEvaluationModule(ctx context.Context, db store.DB, executor egress.Executor) (*EvaluationModule, error) {
	if db == nil {
		return nil, errors.New("evaluation requires caller-supplied isolated storage")
	}
	data, err := NewPostgresDataStore(db, PlacementKB)
	if err != nil {
		return nil, err
	}
	backend := data.(*postgresDataStore)
	handler := NewHandler(executor, WithDataStore(PlacementKB, data), func(options *handlerOptions) { options.dataContext = ctx })
	return &EvaluationModule{ctx: ctx, backend: backend, db: db, Handler: handler}, nil
}

func (m *EvaluationModule) Call(stage uint32, verb string, request any, target any) error {
	ctx, handler := m.ctx, m.Handler
	if err := ctx.Err(); err != nil {
		return err
	}
	raw, err := json.Marshal(request)
	if err != nil {
		return err
	}
	if stage == StageCommand {
		raw, err = bus.EncodeCommand(verb, raw)
		if err != nil {
			return err
		}
	}
	raw, status := handler(bus.ModuleInvocation{StageID: stage}, raw)
	if status != bus.ModuleStatusOK {
		return fmt.Errorf("evaluation %s: owner status %d", verb, status)
	}
	if stage == StageCommand {
		raw, err = bus.DecodeCommandResult(raw)
		if err != nil {
			return err
		}
	}
	var receipt struct {
		Status  string `json:"status"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(raw, &receipt); err != nil {
		return err
	}
	if receipt.Status == "error" {
		return fmt.Errorf("evaluation %s: %s", verb, receipt.Message)
	}
	return json.Unmarshal(raw, target)
}

func (m *EvaluationModule) Seed(fixtures []EvaluationFixture, command string) (map[string]string, error) {
	ctx, backend := m.ctx, m.backend
	db := m.db
	if strings.TrimSpace(command) == "" {
		return nil, errors.New("evaluation requires an embedder")
	}
	backend.requireSemantic = true
	corpus := EvaluationCorpus{Version: 1, Fixtures: fixtures}
	if err := corpus.ValidateFixtures(); err != nil {
		return nil, err
	}
	ids := make(map[string]string)
	unique := make(map[int64]bool)
	confidence := .9
	for _, f := range corpus.Fixtures {
		var response DataResponse
		err := m.Call(StageData, "seed", DataRequest{Operation: "insert-epistemic", Tier: f.Tier, Kind: f.Kind, Key: f.Key, Content: f.Content, Confidence: &confidence, SessionID: "corpus", EpistemicKind: "world_fact", IncludeAll: true}, &response)
		if err != nil {
			return nil, err
		}
		if len(response.Records) != 1 || response.Records[0].ID <= 0 || unique[response.Records[0].ID] {
			return nil, errors.New("evaluation fixture was rejected or aliases an existing fixture")
		}
		id := response.Records[0].ID
		ids[f.FID], unique[id] = strconv.FormatInt(id, 10), true
	}
	// Drain metadata with the same transaction/context and derivation as the
	// production worker. Finish this before freezing the version's input set.
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		tx, err := db.Begin(ctx)
		if err != nil {
			return nil, err
		}
		worked := false
		err = func() error {
			defer tx.Rollback(context.Background())
			if err := sharedIndexContext(ctx, tx); err != nil {
				return err
			}
			bound := *backend
			bound.db = tx
			var err error
			worked, err = bound.indexSharedRecord(ctx)
			if err != nil {
				return err
			}
			return tx.Commit(ctx)
		}()
		if err != nil {
			return nil, err
		}
		if !worked {
			break
		}
	}
	var ready struct {
		Ready  bool `json:"ready"`
		Failed int  `json:"failed"`
	}
	if err := m.Call(StageCommand, "reembed_start", map[string]any{"version": "evaluation", "embedding_command": command}, &ready); err != nil {
		return nil, err
	}
	if !ready.Ready || ready.Failed != 0 {
		return nil, fmt.Errorf("evaluation corpus embedding incomplete (ready=%t, failed=%d)", ready.Ready, ready.Failed)
	}
	var activated struct {
		Status string `json:"status"`
	}
	if err := m.Call(StageCommand, "reembed_cutover", map[string]any{}, &activated); err != nil {
		return nil, err
	}
	if activated.Status != "ok" {
		return nil, errors.New("evaluation embedding activation failed")
	}
	return ids, nil
}
