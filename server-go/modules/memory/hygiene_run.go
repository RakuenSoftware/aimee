package memory

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strconv"
	"time"
)

const hygienePolicy = "bounded-proposal-hygiene-v1"

type hygieneCursor struct {
	Schema     int    `json:"schema_version"`
	Owner      string `json:"owner_id"`
	Scope      Scope  `json:"scope"`
	Generation string `json:"generation"`
	After      string `json:"after_id"`
	Policy     string `json:"policy"`
}

func decodeHygieneCursor(raw string, scope Scope) (hygieneCursor, int64, error) {
	if raw == "" {
		return hygieneCursor{}, 0, nil
	}
	if len(raw) > 4096 {
		return hygieneCursor{}, 0, errors.New("memory: invalid hygiene cursor")
	}
	body, err := base64.RawURLEncoding.DecodeString(raw)
	var c hygieneCursor
	if err != nil || json.Unmarshal(body, &c) != nil || c.Schema != 1 || c.Policy != hygienePolicy || c.Scope != scope || c.Owner == "" || c.Generation == "" {
		return c, 0, errors.New("memory: hygiene cursor scope or policy mismatch")
	}
	id, err := strconv.ParseInt(c.After, 10, 64)
	if err != nil || id < 1 || strconv.FormatInt(id, 10) != c.After {
		return c, 0, errors.New("memory: invalid hygiene cursor position")
	}
	return c, id, nil
}
func encodeHygieneCursor(owner string, scope Scope, generation string, after int64) string {
	raw, _ := json.Marshal(hygieneCursor{1, owner, scope, generation, strconv.FormatInt(after, 10), hygienePolicy})
	return base64.RawURLEncoding.EncodeToString(raw)
}

// The detector transaction drops canonical mutation privileges. Its only write
// capability is the learning owner's narrowly typed proposal-admission function.
// Restore the prior role before releasing a fixture savepoint or pooled transaction.
func (s *postgresDataStore) hygieneWithWorker(ctx context.Context, scope Scope, request *hygienePreviewRequest) (out hygienePreview, err error) {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	var prior string
	if err = s.db.QueryRow(ctx, `SELECT current_user::text`).Scan(&prior); err != nil {
		return out, err
	}
	if _, err = s.db.Exec(ctx, `SET LOCAL ROLE aimee_memory_hygiene`); err != nil {
		return out, err
	}
	defer func() {
		if err != nil {
			return
		} // The enclosing rollback restores the role on failure.
		_, restore := s.db.Exec(ctx, `SELECT set_config('role',$1,true)`, prior)
		if err == nil {
			err = restore
		}
	}()
	out, err = s.previewHygiene(ctx, scope, request)
	if err != nil || request.DryRun {
		return out, err
	}
	out.RunID = releaseDigest([]any{hygienePolicy, scope, out.OwnerID, out.Generation, request})
	proof, _ := json.Marshal(map[string]any{"run_key": out.RunID, "scope": scope, "owner_id": out.OwnerID, "generation": out.Generation, "policy": hygienePolicy})
	if err = s.db.QueryRow(ctx, `SELECT learning_hygiene_job($1::jsonb)`, string(proof)).Scan(&out.JobID); err != nil {
		return out, err
	}
	out.TelemetryWrites = true
	for i := range out.Findings {
		raw, e := json.Marshal(map[string]any{"policy": hygienePolicy, "scope": scope, "owner_id": out.OwnerID, "finding": out.Findings[i]})
		if e != nil {
			return out, e
		}
		var reply string
		if e = s.db.QueryRow(ctx, `SELECT learning_hygiene_queue($1::jsonb)::text`, string(raw)).Scan(&reply); e != nil {
			return out, e
		}
		var proposal struct {
			ID      string `json:"proposal_id"`
			State   string `json:"state"`
			Created bool   `json:"created"`
		}
		if json.Unmarshal([]byte(reply), &proposal) != nil || proposal.ID == "" {
			return out, errors.New("memory: invalid hygiene admission receipt")
		}
		out.Findings[i].ProposalID = proposal.ID
		out.Findings[i].ProposalState = proposal.State
		if proposal.Created {
			out.ProposalWrites++
		}
	}
	_, err = s.db.Exec(ctx, `SELECT learning_hygiene_finish($1::bigint,$2,$3,$4,$5)`, out.JobID, out.Partial, out.ResumeCursor, out.RowsInspected, out.ProposalWrites)
	return out, err
}
