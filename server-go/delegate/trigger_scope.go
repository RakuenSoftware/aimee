package delegate

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"math"
	"strings"
	"unicode/utf8"
)

const EventTriggerScope uint32 = 6680
const StageTriggerScope uint32 = 24
const WorkflowPrincipal uint32 = 64

type TriggerExecutionRegistration struct {
	Origin      string  `json:"origin"`
	Source      string  `json:"source"`
	Event       string  `json:"event"`
	Workspace   string  `json:"workspace"`
	Ref         string  `json:"ref"`
	Pipeline    string  `json:"pipeline"`
	Mode        string  `json:"mode"`
	MaxSpendUSD float64 `json:"max_spend_usd"`
}
type TriggerExecutionScopeRequest struct {
	Version             int                          `json:"version"`
	Registration        TriggerExecutionRegistration `json:"registration"`
	ConfigurationSHA256 string                       `json:"configuration_sha256"`
}
type TriggerExecutionScopeReply struct {
	ExecutionScope string `json:"execution_scope"`
}

func (r TriggerExecutionScopeRequest) Valid() bool {
	p := r.Registration
	if r.Version != WireVersion || (p.Origin != "config" && p.Origin != "workflow") || (p.Source != "watch-dir" && p.Source != "proposals") || p.Workspace == "" || p.Pipeline == "" || p.MaxSpendUSD < 0 || math.IsNaN(p.MaxSpendUSD) || math.IsInf(p.MaxSpendUSD, 0) {
		return false
	}
	for _, f := range []struct {
		v string
		n int
	}{{p.Origin, 32}, {p.Source, 64}, {p.Event, 1024}, {p.Workspace, 4095}, {p.Ref, 256}, {p.Pipeline, 128}, {p.Mode, 32}} {
		if len(f.v) > f.n || !utf8.ValidString(f.v) || strings.ContainsRune(f.v, 0) {
			return false
		}
	}
	body, err := json.Marshal(p)
	if err != nil {
		return false
	}
	hash := sha256.Sum256(body)
	return r.ConfigurationSHA256 == hex.EncodeToString(hash[:])
}
func (c *BusClient) ResolveTriggerExecutionScope(ctx context.Context, request TriggerExecutionScopeRequest) (string, error) {
	request.Version = WireVersion
	if !request.Valid() {
		return "", errors.New("invalid registered trigger source")
	}
	body, err := json.Marshal(request)
	if err != nil {
		return "", err
	}
	reply, err := c.caller.Call(ctx, EventTriggerScope, StageTriggerScope, c.trace.Add(1), c.deadline, body)
	if err != nil {
		return "", err
	}
	var scope TriggerExecutionScopeReply
	if !canonical.Decode(reply, &scope) || !ValidExecutionScope(scope.ExecutionScope) {
		return "", errors.New("host returned invalid trigger execution scope")
	}
	return scope.ExecutionScope, nil
}
