package postgres

import (
	"context"
	"fmt"
	"sync/atomic"
	"testing"
	"time"

	contract "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
)

type sharedLifecycleCaller struct{ handler bus.ModuleHandler }

func (c sharedLifecycleCaller) Call(ctx context.Context, kind, stage uint32, _ uint64, _ time.Duration, request []byte) ([]byte, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if kind != families.Lifecycle.Event || stage != families.Lifecycle.Stage {
		return nil, fmt.Errorf("unexpected lifecycle route")
	}
	body, status := c.handler(bus.ModuleInvocation{StageID: stage}, request)
	if status != bus.ModuleStatusOK {
		return nil, fmt.Errorf("lifecycle transport status %d", status)
	}
	return body, nil
}

func sharedLifecycleClient(t *testing.T, store db.Store) *contract.Client {
	t.Helper()
	client, err := contract.NewClient(sharedLifecycleCaller{families.Lifecycle.Handler(store)}, 0)
	if err != nil {
		t.Fatal(err)
	}
	return client
}

func sharedCreateWork(client *contract.Client, id, parent string, cap int) (int, error) {
	return client.WfeCreateWorkItem(context.Background(), id, "repo", id, "build", "v1", "impl", "autonomous", "test", parent, "", 1, cap)
}

func TestSharedDatabaseWorkflowAdmissionAcrossProviders(t *testing.T) {
	pool := sharedTestDatabase(t)
	store := sharedTestStore(t, pool)
	if err := families.ApplySchema(context.Background(), store); err != nil {
		t.Fatal(err)
	}
	clients := []*contract.Client{sharedLifecycleClient(t, store), sharedLifecycleClient(t, sharedTestStore(t, pool))}
	var admitted atomic.Int32
	sharedParallel(t, 12, func(i int) error {
		outcome, err := sharedCreateWork(clients[i%2], fmt.Sprintf("root-%d", i), "", 2)
		if err != nil {
			return err
		}
		if outcome == 0 {
			admitted.Add(1)
		} else if outcome != 1 {
			return fmt.Errorf("unexpected admission result %d", outcome)
		}
		return nil
	})
	if got := admitted.Load(); got != 2 {
		t.Fatalf("admitted %d roots at cap 2", got)
	}
}

func TestSharedDatabaseWorkflowSiblingClaimsAcrossProviders(t *testing.T) {
	pool := sharedTestDatabase(t)
	store := sharedTestStore(t, pool)
	if err := families.ApplySchema(context.Background(), store); err != nil {
		t.Fatal(err)
	}
	clients := []*contract.Client{sharedLifecycleClient(t, store), sharedLifecycleClient(t, sharedTestStore(t, pool))}
	for _, item := range []struct{ id, parent string }{{"root", ""}, {"left", "root"}, {"right", "root"}} {
		if outcome, err := sharedCreateWork(clients[0], item.id, item.parent, 0); err != nil || outcome != 0 {
			t.Fatalf("create: %d: %v", outcome, err)
		}
	}
	var winners, conflicts atomic.Int32
	sharedParallel(t, 2, func(i int) error {
		id := []string{"left", "right"}[i]
		result, err := clients[i].WfeClaimFrozenCreates(context.Background(), "root", id, 1,
			[]contract.WfeClaimFrozenCreatesItem{{Path: "new.go", ContentHash: id}})
		if err != nil {
			return err
		}
		if result.Path == "" {
			winners.Add(1)
		} else {
			conflicts.Add(1)
		}
		return nil
	})
	if winners.Load() != 1 || conflicts.Load() != 1 {
		t.Fatalf("winners=%d conflicts=%d", winners.Load(), conflicts.Load())
	}
}

func TestSharedDatabaseWorkflowStopAndChildCreation(t *testing.T) {
	pool := sharedTestDatabase(t)
	store := sharedTestStore(t, pool)
	ctx := context.Background()
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	clients := []*contract.Client{sharedLifecycleClient(t, store), sharedLifecycleClient(t, sharedTestStore(t, pool))}
	for attempt := 0; attempt < 20; attempt++ {
		root := fmt.Sprintf("root-%d", attempt)
		if outcome, err := sharedCreateWork(clients[0], root, "", 0); err != nil || outcome != 0 {
			t.Fatalf("create: %d: %v", outcome, err)
		}
		sharedParallel(t, 2, func(i int) error {
			if i == 0 {
				_, err := clients[0].WfeStopTree(ctx, root, 512)
				return err
			}
			outcome, err := sharedCreateWork(clients[1], root+".child", root, 0)
			if err != nil {
				return err
			}
			if outcome != 0 && outcome != 2 {
				return fmt.Errorf("unexpected child admission %d", outcome)
			}
			return nil
		})
		var active int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM lifecycle_work_item WHERE parent_id=$1 AND state='active'", root).Scan(&active); err != nil {
			t.Fatal(err)
		}
		if active != 0 {
			t.Fatal("stop/create race left an active orphan")
		}
	}
}

func TestSharedDatabaseWorkflowBudgetReplyOrder(t *testing.T) {
	pool := sharedTestDatabase(t)
	store := sharedTestStore(t, pool)
	ctx := context.Background()
	if err := families.ApplySchema(ctx, store); err != nil {
		t.Fatal(err)
	}
	client := sharedLifecycleClient(t, store)
	if outcome, err := sharedCreateWork(client, "root", "", 0); err != nil || outcome != 0 {
		t.Fatalf("create: %d: %v", outcome, err)
	}
	if _, err := pool.Exec(ctx, "UPDATE lifecycle_work_item SET cum_cost_usd=0.75 WHERE work_item_id='root'"); err != nil {
		t.Fatal(err)
	}
	totals, err := client.WfeBudgetTotals(ctx, "root")
	if err != nil || totals.RootID != "root" || totals.Spent != 0.75 || totals.MaxUSD != 1 {
		t.Fatalf("budget reply: %+v: %v", totals, err)
	}
}
