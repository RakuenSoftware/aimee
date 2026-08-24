package families

import (
	"context"
	"testing"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// A variadic operation must survive a frame narrower than it reads.
//
// Args: -1 tells dispatch not to check the field count, so these are the only
// operations a caller can reach with any width at all -- family.go refuses a
// wrong count before any handler runs, but only when the count is FIXED.
// Whatever bound a variadic operation needs, it has to impose itself.
//
// The interesting widths are at the small end. An operation that reads f[0] and
// f[1] is reached by a 0-cell frame with an index panic, not a refusal, and a
// panic in a handler takes the module down rather than answering the caller.
// (The peer-messaging session found its arity cases the same way, at 0 and 1
// rather than at the top of the range.)
//
// THE SET IS DERIVED, NOT LISTED. An earlier version named the five operations
// it knew about, which is an enumerated gate: a variadic operation added
// tomorrow is outside a hand-written map and nothing says so. Walking All() for
// Args < 0 means a new one is covered by existing code, and the only way to
// leave this test is to stop being variadic.

// stubQueryer answers every call successfully and touches nothing.
//
// A stub rather than nil. The first version passed nil to prove no handler
// reaches the store on a narrow frame -- but some legitimately do: a 1-cell
// model_catalog_replace is a valid "this provider now has no models", and a
// 1-cell mark_* is a valid one-id list. Both then dereferenced the nil and the
// test reported a panic that was its own.
//
// What is under test is whether a narrow frame INDEXES out of range, so the
// store only has to be present, not real.
type stubQueryer struct{}

func (stubQueryer) Exec(context.Context, string, ...any) (store.Tag, error) {
	return store.RowsAffected(0), nil
}

func (stubQueryer) Query(context.Context, string, ...any) (store.Rows, error) {
	return nil, store.ErrNoRows
}

func (stubQueryer) QueryRow(context.Context, string, ...any) store.Row { return stubRow{} }

type stubRow struct{}

func (stubRow) Scan(...any) error { return store.ErrNoRows }

// stubDB is stubQueryer plus a Begin that refuses, for the RunDB operations.
// Refusing rather than handing back a fake transaction: an operation that gets
// as far as Begin has already read its fields, which is the whole question
// here, and a fake Tx would only invite assertions about what it committed.
type stubDB struct{ stubQueryer }

func (stubDB) Begin(context.Context) (store.Tx, error) { return nil, store.ErrNoRows }

func TestVariadicOperationsSurviveAFrameNarrowerThanTheyRead(t *testing.T) {
	narrow := [][]string{
		{},
		{""},
		{"1"},
	}

	type variadicOp struct {
		family string
		name   string
		run    func(context.Context, []string) (uint32, []string, error)
	}
	var ops []variadicOp
	for _, fam := range All() {
		for _, spec := range fam.Ops {
			if spec.Args >= 0 {
				continue
			}
			spec := spec
			switch {
			case spec.Run != nil:
				ops = append(ops, variadicOp{fam.Name, spec.Name,
					func(ctx context.Context, f []string) (uint32, []string, error) {
						return spec.Run(ctx, stubQueryer{}, f)
					}})
			case spec.RunDB != nil:
				ops = append(ops, variadicOp{fam.Name, spec.Name,
					func(ctx context.Context, f []string) (uint32, []string, error) {
						return spec.RunDB(ctx, stubDB{}, f)
					}})
			default:
				t.Errorf("%s/%s is variadic but has neither Run nor RunDB",
					fam.Name, spec.Name)
			}
		}
	}

	// A refusal test that refuses nothing passes trivially, and the one thing
	// it cannot establish by observing refusals is whether any were requested.
	// If a rename or a signature change empties this set, the loop below runs
	// zero times and reports success having exercised nothing.
	if len(ops) == 0 {
		t.Fatal("no operation in All() declares Args < 0. Either every variadic " +
			"operation was retired -- in which case delete this test and say so -- " +
			"or the walk stopped matching and this passed having tested nothing.")
	}
	if len(narrow) == 0 {
		t.Fatal("no narrow frames to send")
	}

	exercised := 0
	for _, op := range ops {
		for _, fields := range narrow {
			t.Run(op.family+"/"+op.name, func(t *testing.T) {
				defer func() {
					if r := recover(); r != nil {
						t.Fatalf("%s panicked on a %d-cell frame: %v\n"+
							"    Args -1 means dispatch does not check the width, so "+
							"this width is reachable from the wire.",
							op.name, len(fields), r)
					}
				}()
				status, cells, err := op.run(context.Background(), fields)
				if err != nil {
					t.Fatalf("%s on a %d-cell frame returned an error rather than "+
						"refusing: %v", op.name, len(fields), err)
				}
				// Some narrow frames are legitimate rather than malformed: an
				// empty id list means "mark nothing", and a 1-cell catalog
				// replace means "this provider now has no models". Those are
				// answers, and the property under test is that no width panics.
				if status == store.StatusOK {
					return
				}
				if status != store.StatusInvalid {
					t.Errorf("%s on a %d-cell frame answered status %d, want "+
						"StatusInvalid (%d); cells=%v",
						op.name, len(fields), status, store.StatusInvalid, cells)
				}
			})
			exercised++
		}
	}
	t.Logf("%d variadic operation(s) x %d narrow frame(s) = %d case(s) exercised",
		len(ops), len(narrow), exercised)
}
