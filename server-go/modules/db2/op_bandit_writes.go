package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageBanditArmStatsUpdate,
		db2contract.OperationBanditArmStatsUpdate, banditArmStatsUpdate)
	Register(db2contract.StageBanditPromotionSet,
		db2contract.OperationBanditPromotionSet, banditPromotionSet)
	Register(db2contract.StageBanditExploreStats,
		db2contract.OperationBanditExploreStats, banditExploreStats)
}

// The casts are here for the C, not for this module.
//
// The C connection calls PQexecParams with paramTypes NULL, so the server has
// to infer every parameter type from context -- and it cannot infer the type of
// a product of two unknowns: "operator is not unique: unknown * unknown". The C
// wrote reward_delta * reward_delta bare, so the statement was rejected every
// time it was issued and no arm ever accumulated a decision, a reward or a
// posterior. Nothing noticed because the step result was discarded and the
// caller was told the write had succeeded.
//
// pgx sends parameter types, so this module runs the untyped form perfectly
// well -- verified by removing the casts and watching the live probe still
// pass. They are kept anyway for two reasons: the two statements should stay
// textually comparable for the parity gate, and an untyped product here is one
// copy-paste away from being untyped in the C again.
//
// sum_reward_sq is the sum of squares, kept alongside the sum so a variance can
// be computed without holding every reward. That is why the parameter appears
// squared rather than a second value being passed: the two must be the same
// number or the variance is of nothing.
const banditArmStatsUpdateQuery = `INSERT INTO bandit_arm_stats
 (decision_point, arm_id, n_decisions, n_rewards, sum_reward, sum_reward_sq,
  posterior_alpha, posterior_beta, updated_at)
 VALUES ($1, $2, 1, 1, $3::double precision,
         $3::double precision * $3::double precision, $4, $5,
         to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'))
 ON CONFLICT (decision_point, arm_id) DO UPDATE
 SET n_decisions = bandit_arm_stats.n_decisions + 1,
     n_rewards = bandit_arm_stats.n_rewards + 1,
     sum_reward = bandit_arm_stats.sum_reward + $3::double precision,
     sum_reward_sq = bandit_arm_stats.sum_reward_sq
                     + $3::double precision * $3::double precision,
     posterior_alpha = $4,
     posterior_beta = $5,
     updated_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC',
                          'YYYY-MM-DD"T"HH24:MI:SS"Z"')`

// banditArmStatsUpdate folds one observed reward into an arm's running
// statistics.
//
// The counters accumulate and the posteriors are replaced. That asymmetry is
// deliberate: the sums are evidence, which only grows, while the posterior is
// the caller's current belief given all of it, which it recomputes and hands
// over whole.
//
// Unlike the C this reports whether the statement ran. Acknowledging a write
// that failed is how the broken statement above stayed invisible.
func banditArmStatsUpdate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionPoint, armID, reward, alpha, beta, err :=
		db2contract.DecodeBanditArmStatsUpdateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, banditArmStatsUpdateQuery,
		decisionPoint, armID, reward, alpha, beta)
	return acknowledgement(execErr == nil, db2contract.EncodeBanditArmStatsUpdateReply)
}

const banditPromotionSetQuery = `INSERT INTO bandit_promotions
 (decision_point, arm_id, rollback_arm, promoted_at)
 VALUES ($1, $2, $3,
         to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'))
 ON CONFLICT (decision_point) DO UPDATE
 SET arm_id = $2, rollback_arm = $3,
     promoted_at = to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC',
                           'YYYY-MM-DD"T"HH24:MI:SS"Z"')`

// banditPromotionSet pins a decision point to one arm, recording what to fall
// back to.
//
// One promotion per decision point, enforced by the primary key: promoting
// again replaces rather than accumulates, because a decision point pinned to
// two arms is not pinned. The rollback arm is stored beside it so undoing the
// promotion does not need to remember what came before.
func banditPromotionSet(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionPoint, armID, rollbackArm, err :=
		db2contract.DecodeBanditPromotionSetRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	_, execErr := store.Exec(ctx, banditPromotionSetQuery, decisionPoint, armID, rollbackArm)
	return acknowledgement(execErr == nil, db2contract.EncodeBanditPromotionSetReply)
}

// The cutoff is computed in the statement rather than in Go, so the window is
// measured against the database's clock -- the same one decided_at was stamped
// from. A window of zero or less becomes the empty string, which every
// timestamp compares greater than, so it means "any time" rather than "none".
//
// The comparison is lexicographic over ISO-8601 UTC text, which is why the
// format has to match exactly what the decision writer stamps.
const banditExploreStatsQuery = `SELECT COUNT(*),
 SUM(CASE WHEN is_exploration THEN 1 ELSE 0 END)
 FROM bandit_decisions
 WHERE decision_point = $1
   AND decided_at >= CASE WHEN $2 > 0
       THEN to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC' - make_interval(secs => $2),
                    'YYYY-MM-DD"T"HH24:MI:SS"Z"')
       ELSE '' END`

// banditExploreStats reports how many decisions in a window were exploratory.
//
// Both numbers are returned rather than a ratio, because the caller needs to
// know the denominator: one exploration out of two is not the same evidence as
// five hundred out of a thousand, and a ratio hides that.
//
// SUM over no rows is NULL while COUNT is zero, so the two columns scan
// differently even though both are counts.
func banditExploreStats(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	decisionPoint, windowSeconds, err := db2contract.DecodeBanditExploreStatsRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var total, explore *int64
	if scanErr := store.QueryRow(ctx, banditExploreStatsQuery,
		decisionPoint, int64(windowSeconds)).Scan(&total, &explore); scanErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := db2contract.EncodeBanditExploreStatsReply(
		clampToU64(number(explore)), clampToU64(number(total)))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
