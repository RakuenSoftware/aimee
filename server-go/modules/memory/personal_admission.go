package memory

import (
	"context"
	"errors"
	"strconv"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// PersonalAuthorship describes verified creation or correction, never inferred
// from private placement or an authority field supplied in the record body.
type PersonalAuthorship struct {
	Reviewer        string `json:"reviewer,omitempty"`
	ReviewTransport string `json:"review_transport,omitempty"`
	ProposalID      string `json:"proposal_id,omitempty"`
	Category        string `json:"category"`
	Principal       string `json:"principal,omitempty"`
	Transport       string `json:"transport,omitempty"`
}
type personalActor struct {
	authority            int
	principal, transport string
}

func personalCaller(caller *bus.CommandContext, requested int) personalActor {
	actor := personalActor{authority: AuthorityModel, principal: "system:model-inference", transport: "internal"}
	if caller != nil && caller.Authenticated && caller.Principal != "" {
		actor.principal, actor.transport = caller.Principal, caller.TransportIdentity
		if caller.UserAuthority && requested == AuthorityUser {
			actor.authority = AuthorityUser
		}
	}
	return actor
}

// Admission, version comparison, payload, provenance, history and journal commit
// together. Store additionally serializes a missing (kind,key), so two callers
// cannot bypass replacement admission by racing an insert.
func (s *postgresDataStore) mutatePersonal(ctx context.Context, operation string, wanted Record, expected *MemoryRecordVersion) (Record, error) {
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return Record{}, err
		}
		defer tx.Rollback(context.WithoutCancel(ctx))
		bound := *s
		bound.db = tx
		out, err := bound.mutatePersonal(ctx, operation, wanted, expected)
		if err != nil && proposedCorrection(err) == nil {
			return Record{}, err
		}
		outcomeErr := err
		if err = tx.Commit(ctx); err != nil {
			return Record{}, err
		}
		return out, outcomeErr
	}
	if _, ok := s.db.(store.Tx); !ok {
		return Record{}, errors.New("memory: private mutation requires a transaction")
	}
	actor := s.personalActor
	if actor.principal == "" {
		actor = personalCaller(nil, AuthorityModel)
	}
	authority, category := "model", "agent_message"
	if actor.authority == AuthorityUser {
		authority, category = "user", "user_stated"
	} else {
		wanted.Confidence = min(wanted.Confidence, .8)
		if wanted.Tier == "L5" {
			wanted.Confidence = min(wanted.Confidence, .5)
		}
	}
	settings := `SELECT set_config('aimee.private_authority',$1,true),set_config('aimee.private_principal',$2,true),set_config('aimee.private_transport',$3,true)`
	args := []any{authority, actor.principal, actor.transport}
	if operation == "store" {
		settings += `,pg_advisory_xact_lock(hashtextextended(json_build_array($4::text,$5::text)::text,5750))`
		args = append(args, wanted.Kind, wanted.Key)
	}
	if _, err := s.db.Exec(ctx, settings, args...); err != nil {
		return Record{}, err
	}
	old := Record{Scope: wanted.Scope, Authorship: &PersonalAuthorship{}}
	var state, revision, owner string
	var unbounded, current bool
	predicate, params := "id=$1", []any{wanted.ID}
	if operation == "store" {
		predicate, params = "kind=$1 AND key=$2", []any{wanted.Kind, wanted.Key}
	}
	err := s.db.QueryRow(ctx, `SELECT id,tier,kind,key,content,confidence,lifecycle_state,
 valid_until IS NULL,(valid_until IS NULL OR valid_until>now()),record_revision::text,
 provenance_category,author_principal,author_transport,reviewer_principal,reviewer_transport,review_proposal_id,(SELECT owner_id::text FROM user_memory_collection_generation WHERE id=1) FROM user_memories WHERE `+predicate+` FOR NO KEY UPDATE`, params...).Scan(
		&old.ID, &old.Tier, &old.Kind, &old.Key, &old.Content, &old.Confidence, &state, &unbounded, &current, &revision,
		&old.Authorship.Category, &old.Authorship.Principal, &old.Authorship.Transport, &old.Authorship.Reviewer, &old.Authorship.ReviewTransport, &old.Authorship.ProposalID, &owner)
	if store.IsNoRows(err) {
		if operation != "store" {
			return Record{}, ErrMemoryNotFound
		}
		wanted.Authorship = &PersonalAuthorship{}
		err = s.db.QueryRow(ctx, `INSERT INTO user_memories(kind,tier,key,content,confidence,updated_at)
 VALUES($1,$2,$3,$4,$5,now()) RETURNING id,confidence,provenance_category,author_principal,author_transport,reviewer_principal,reviewer_transport,review_proposal_id`,
			wanted.Kind, wanted.Tier, wanted.Key, wanted.Content, wanted.Confidence).Scan(&wanted.ID, &wanted.Confidence,
			&wanted.Authorship.Category, &wanted.Authorship.Principal, &wanted.Authorship.Transport, &wanted.Authorship.Reviewer, &wanted.Authorship.ReviewTransport, &wanted.Authorship.ProposalID)
		return wanted, err
	}
	if err != nil {
		return Record{}, err
	}
	if operation != "store" && (state != "active" || (operation == "supersede" && !current)) {
		return Record{}, ErrMemoryNotFound
	}
	if expected != nil {
		if expected.OwnerID != owner || expected.RecordRevision != revision {
			return Record{}, errMutationVersionConflict
		}
	}
	if operation == "store" && !current && actor.authority != AuthorityUser {
		return Record{}, errMutationReviewRequired
	}
	if operation == "store" && state != "active" && (actor.authority != AuthorityUser || (state != "retired" && state != "expired")) {
		return Record{}, errMutationReviewRequired
	}
	if operation == "delete" {
		if actor.authority != AuthorityUser {
			if err := admitMemoryReplacement(old.Kind, old.Authorship.Category, actor.authority); err != nil {
				return Record{}, err
			}
		}
		if expected == nil {
			_, err = s.db.Exec(ctx, `UPDATE user_memories SET lifecycle_state='retired',updated_at=now() WHERE id=$1`, old.ID)
		} else {
			err = s.db.QueryRow(ctx, `UPDATE user_memories SET lifecycle_state='retired',updated_at=now() WHERE id=$1 RETURNING record_revision::text`, old.ID).Scan(&revision)
			if err == nil {
				old.Version = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: strconv.FormatInt(old.ID, 10), RecordRevision: revision}
			}
		}
		return old, err
	}
	if operation == "supersede" {
		wanted.Kind, wanted.Key, wanted.Tier = old.Kind, old.Key, old.Tier
		if actor.authority != AuthorityUser && wanted.Tier == "L5" {
			wanted.Confidence = min(wanted.Confidence, .5)
		}
	}
	unchanged := wanted.Kind == old.Kind && wanted.Key == old.Key && wanted.Tier == old.Tier && wanted.Content == old.Content && wanted.Confidence == old.Confidence && state == "active" && (operation != "store" || unbounded)
	if !unchanged || old.Authorship.Category != category {
		if err := admitMemoryReplacement(old.Kind, old.Authorship.Category, actor.authority); err != nil {
			if errors.Is(err, errMutationReviewRequired) && actor.authority == AuthorityModel && state == "active" && current {
				return Record{}, s.proposePersonalCorrection(ctx, old, wanted)
			}
			return Record{}, err
		}
	}
	wanted.ID, wanted.Authorship = old.ID, &PersonalAuthorship{}
	assignments := "tier=$2,content=$3,confidence=$4,updated_at=now()"
	if operation == "store" {
		assignments += ",lifecycle_state='active',valid_until=NULL"
	}
	err = s.db.QueryRow(ctx, `UPDATE user_memories SET `+assignments+` WHERE id=$1
 RETURNING record_revision::text,confidence,provenance_category,author_principal,author_transport,reviewer_principal,reviewer_transport,review_proposal_id`, old.ID, wanted.Tier, wanted.Content, wanted.Confidence).Scan(
		&revision, &wanted.Confidence, &wanted.Authorship.Category, &wanted.Authorship.Principal, &wanted.Authorship.Transport, &wanted.Authorship.Reviewer, &wanted.Authorship.ReviewTransport, &wanted.Authorship.ProposalID)
	if expected != nil {
		wanted.Version = &MemoryRecordVersion{SchemaVersion: 1, OwnerID: expected.OwnerID, RecordID: strconv.FormatInt(old.ID, 10), RecordRevision: revision}
	}
	return wanted, err
}
