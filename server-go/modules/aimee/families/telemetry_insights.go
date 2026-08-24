package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Insights: three cross-table roll-ups that answer "where is the work going".
//
// These read tables the telemetry family does not own -- server_sessions and
// delegation_spawns -- which is why they are here rather than beside the token
// audit: they belong to whoever is asking the question, not to the table.

// Reply widths, from the catalog.
const (
	insightsPlatformCells = 2
	insightsSessionCells  = 7
	insightsDelegateCells = 3
)

const (
	insightsByPlatformSQL = `SELECT client_type, COUNT(*)
	                           FROM server_sessions
	                          WHERE ` + sinceFilter + `
	                          GROUP BY client_type
	                          ORDER BY COUNT(*) DESC, client_type
	                          LIMIT $2`

	// The costliest sessions, with the title the session carries if it has one.
	//
	// Grouped by session_id and NOT by client: two clients that both call
	// themselves "webchat" but carry different session keys are different
	// sessions, and collapsing them would attribute one's spend to the other.
	insightsTopSessionsSQL = `SELECT ta.session_id,
	                                 COALESCE(MAX(ss.title), ''),
	                                 COALESCE(MAX(ta.model), ''),
	                                 COALESCE(SUM(ta.prompt_tokens), 0),
	                                 COALESCE(SUM(ta.completion_tokens), 0),
	                                 COALESCE(SUM(ta.estimated_cost_usd), 0),
	                                 COALESCE(to_char(MIN(ta.created_at) AT TIME ZONE 'utc',
	                                          'YYYY-MM-DD HH24:MI:SS'), '')
	                            FROM token_audit ta
	                            LEFT JOIN server_sessions ss ON ss.id = ta.session_id
	                           WHERE (ta.usage_kind = 'realized' OR ta.usage_kind = '')
	                             AND ($1 <= 0 OR ta.created_at >= now()
	                                             - make_interval(hours => $1::int))
	                           GROUP BY ta.session_id
	                           ORDER BY SUM(ta.estimated_cost_usd) DESC, ta.session_id
	                           LIMIT $2`

	insightsDelegatesByRoleSQL = `SELECT role, COUNT(*),
	                                     COUNT(*) FILTER (WHERE status = 'completed')
	                                FROM delegation_spawns
	                               WHERE ` + sinceFilter + `
	                               GROUP BY role
	                               ORDER BY COUNT(*) DESC, role
	                               LIMIT $2`
)

// sinceAndMax reads the two fields every insight takes.
func sinceAndMax(f []string) (since, max int, ok bool) {
	since, ok = store.Atoi(f[0])
	if !ok {
		return 0, 0, false
	}
	max, ok = boundedMax(f[1])
	return since, max, ok
}

func insightsByPlatform(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, max, ok := sinceAndMax(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, insightsByPlatformSQL, insightsPlatformCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				platform string
				count    int64
			)
			if err := scan(&platform, &count); err != nil {
				return nil, err
			}
			return []string{platform, store.I64toa(count)}, nil
		}, since, max)
}

func insightsTopSessions(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, max, ok := sinceAndMax(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, insightsTopSessionsSQL, insightsSessionCells,
		func(scan func(...any) error) ([]string, error) {
			var (
				sessionID, title, model, createdAt string
				prompt, completion                 int64
				cost                               float64
			)
			if err := scan(&sessionID, &title, &model, &prompt, &completion,
				&cost, &createdAt); err != nil {
				return nil, err
			}
			return []string{
				sessionID, title, model,
				store.I64toa(prompt), store.I64toa(completion),
				store.Ftoa(cost), createdAt,
			}, nil
		}, since, max)
}

func insightsDelegatesByRole(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	since, max, ok := sinceAndMax(f)
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	return collect(ctx, q, insightsDelegatesByRoleSQL, insightsDelegateCells,
		func(scan func(...any) error) ([]string, error) {
			var role string
			var total, completed int64
			if err := scan(&role, &total, &completed); err != nil {
				return nil, err
			}
			return []string{role, store.I64toa(total), store.I64toa(completed)}, nil
		}, since, max)
}
