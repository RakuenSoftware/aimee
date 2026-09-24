package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Only the driver adapter is test-specific; request transactions, settings and
// scoped SQL are the production Go owner's implementation.
type scopePoolStore struct {
	store.DB
	pool *pgxpool.Pool
}

func (d scopePoolStore) Begin(ctx context.Context) (store.Tx, error) {
	tx, err := d.pool.Begin(ctx)
	if err != nil {
		return nil, err
	}
	return evalQueryer{tx}, nil
}

func TestMemoryScopeConcurrentPoolReuse(t *testing.T) {
	dsn := os.Getenv("AIMEE_MEMORY_EVAL_URL")
	if dsn == "" {
		t.Skip("set AIMEE_MEMORY_EVAL_URL")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	admin, err := pgx.Connect(ctx, dsn)
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close(context.Background())
	name := fmt.Sprintf("memory_scope_pool_%d", time.Now().UnixNano())
	quoted := pgx.Identifier{name}.Sanitize()
	// Commit the isolated fixture so two actual pooled sessions can read it.
	_, err = admin.Exec(ctx, `CREATE ROLE `+quoted+` NOINHERIT NOBYPASSRLS;
 CREATE SCHEMA `+quoted+`;
 CREATE TABLE `+quoted+`.memories(id bigint PRIMARY KEY,scope_type text,scope_value text);
 CREATE TABLE `+quoted+`.memory_scopes(memory_id bigint,scope_type text,scope_value text);
 INSERT INTO `+quoted+`.memories VALUES(1,'project','alpha'),(2,'project','beta');
 ALTER TABLE `+quoted+`.memories ENABLE ROW LEVEL SECURITY;
 CREATE POLICY scoped ON `+quoted+`.memories USING (
 current_setting('aimee.memory_scope_all',true)='1' OR
 (scope_type=current_setting('aimee.memory_scope_type',true) AND
 scope_value=current_setting('aimee.memory_scope_value',true)));
 GRANT USAGE ON SCHEMA `+quoted+` TO `+quoted+`;
 GRANT SELECT ON ALL TABLES IN SCHEMA `+quoted+` TO `+quoted)
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if _, err := admin.Exec(context.Background(), "DROP SCHEMA "+quoted+" CASCADE; DROP ROLE "+quoted); err != nil {
			t.Error(err)
		}
	}()
	config, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		t.Fatal("invalid pool configuration")
	}
	config.MaxConns, config.MinConns = 2, 2
	config.AfterConnect = func(ctx context.Context, c *pgx.Conn) error {
		_, err := c.Exec(ctx, "SET ROLE "+quoted+"; SET search_path="+quoted+",public")
		return err
	}
	pool, err := pgxpool.NewWithConfig(ctx, config)
	if err != nil {
		t.Fatal(err)
	}
	defer pool.Close()
	// Hold both physical sessions simultaneously and verify the runtime identity
	// has neither ownership nor bypass privileges before exercising the handler.
	held := make([]*pgxpool.Conn, 0, 2)
	releaseHeld := func() {
		for _, c := range held {
			c.Release()
		}
		held = held[:0]
	}
	defer releaseHeld()
	for range 2 {
		c, err := pool.Acquire(ctx)
		if err != nil {
			t.Fatal(err)
		}
		held = append(held, c)
		var restricted bool
		err = c.QueryRow(ctx, `SELECT NOT rolsuper AND NOT rolbypassrls AND
   current_user<>pg_get_userbyid((SELECT relowner FROM pg_class WHERE oid='memories'::regclass))
   FROM pg_roles WHERE rolname=current_user`).Scan(&restricted)
		if err != nil || !restricted {
			t.Fatal("runtime identity not restricted", err)
		}
	}
	releaseHeld()
	handler := NewHandler(nil, WithDataStore(PlacementKB, &postgresDataStore{db: scopePoolStore{pool: pool}, placement: PlacementKB}))
	start := make(chan struct{})
	failures := make(chan error, 12)
	var workers sync.WaitGroup
	for worker := range 12 {
		workers.Add(1)
		go func(worker int) {
			defer workers.Done()
			<-start
			project := []string{"alpha", "beta"}[worker%2]
			own := int64(worker%2 + 1)
			for iteration := range 24 {
				// Alternate visible, hidden, empty-scope and rolled-back requests.
				id, scope, want := own, project, 1
				if iteration%4 == 1 {
					id = 3 - own
					want = 0
				}
				if iteration%4 == 2 {
					scope = ""
					want = 0
				}
				if iteration%4 == 3 {
					id = 0
				} // Rejected after request transaction setup.
				request, _ := json.Marshal(DataRequest{Operation: "scope-collect", ID: id, Project: scope})
				raw, status := handler(bus.ModuleInvocation{StageID: StageData}, request)
				if iteration%4 == 3 {
					if status != bus.ModuleStatusInvalidRequest {
						failures <- fmt.Errorf("invalid request accepted: %v", status)
						return
					}
					continue
				}
				var response DataResponse
				if status != bus.ModuleStatusOK || json.Unmarshal(raw, &response) != nil || len(response.Scopes) != want {
					failures <- fmt.Errorf("worker %d iteration %d scope %q: status %v scopes %v", worker, iteration, scope, status, response.Scopes)
					return
				}
				if want == 1 && response.Scopes[0] != (ScopeTag{Type: "project", Value: project}) {
					failures <- fmt.Errorf("worker %d inherited foreign scope: %v", worker, response.Scopes)
					return
				}
			}
		}(worker)
	}
	close(start)
	workers.Wait()
	close(failures)
	for err := range failures {
		t.Error(err)
	}
	// Request-local identity and scope must not survive commit or rollback.
	held = held[:0]
	for range 2 {
		c, err := pool.Acquire(ctx)
		if err != nil {
			t.Fatal(err)
		}
		held = append(held, c)
		var clean bool
		err = c.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.memory_scope_type',true),'')=''
   AND COALESCE(current_setting('aimee.memory_scope_value',true),'')=''
   AND COALESCE(current_setting('aimee.memory_project',true),'')=''
   AND COALESCE(current_setting('aimee.memory_scope_all',true),'')=''
   AND COALESCE(current_setting('aimee.principal',true),'')=''
   AND COALESCE(current_setting('aimee.memory_purpose',true),'')=''
   AND COALESCE(current_setting('aimee.memory_policy_version',true),'')=''
   AND COALESCE(current_setting('aimee.memory_query_mode',true),'')=''
   AND COALESCE(current_setting('aimee.memory_valid_at',true),'')=''
   AND COALESCE(current_setting('aimee.memory_believed_at',true),'')=''`).Scan(&clean)
		if err != nil || !clean {
			t.Error("request context survived pool release", err)
		}
	}
	releaseHeld()
}
