// Command aimee-module is a multicall executable for isolated Go module processes.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db1"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/JBailes/aimee/server-go/modules/aimee/families"
	"github.com/JBailes/aimee/server-go/modules/aimee/peer"
	"github.com/JBailes/aimee/server-go/modules/aimee/peerwire"
	"github.com/JBailes/aimee/server-go/modules/benchmarks"
	controlweb "github.com/JBailes/aimee/server-go/modules/control-web"
	"github.com/JBailes/aimee/server-go/modules/delegates"
	"github.com/JBailes/aimee/server-go/modules/economizer"
	modulegit "github.com/JBailes/aimee/server-go/modules/git"
	"github.com/JBailes/aimee/server-go/modules/governance"
	kbsynthesis "github.com/JBailes/aimee/server-go/modules/kb-synthesis"
	"github.com/JBailes/aimee/server-go/modules/learning"
	mcpmodule "github.com/JBailes/aimee/server-go/modules/mcp"
	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
	responsecomposition "github.com/JBailes/aimee/server-go/modules/response-composition"
	"github.com/JBailes/aimee/server-go/modules/roundtable"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
	"github.com/JBailes/aimee/server-go/modules/routing"
	runtimeweb "github.com/JBailes/aimee/server-go/modules/runtime-web"
	"github.com/JBailes/aimee/server-go/modules/sandbox"
	"github.com/JBailes/aimee/server-go/modules/skills"
	moduletools "github.com/JBailes/aimee/server-go/modules/tools"
	"github.com/JBailes/aimee/server-go/modules/workspace"
)

var errUsage = errors.New("usage: aimee-module-NAME DAEMON_MODULE_BUS_SOCKET")

// roundtableReviewer assembles the review capability from this process's own
// environment: the saved roundtables on disk, and the delegate bus stage it
// seats them over. Both are required -- a review with no configured panel,
// or with no way to reach an agent, is not a degraded review but no review.
const roundtableDelegatePrincipalRef uint32 = 65

// economizerStorePrincipalRef is the economizer's OUTBOUND identity. A module's
// serving grant requests nothing, so reaching another module's stage needs a
// second principal that is granted exactly that request and nothing else.
const economizerStorePrincipalRef uint32 = 66

// storePrincipalRef is the store module's OUTBOUND identity, used to call the
// postgres module. Separate from its serving ref (30) because a serving grant
// requests nothing -- the rule that stops a module's right to answer becoming a
// right to ask.
//
// 69, having been 67 and then 68 in turn. Both were taken by the session
// building peer messaging -- 67 for its directory client below, 68 for the
// server's own peer client -- and it landed first. Two clients on one ref are
// two callers the bus cannot tell apart, and the failure surfaces long after
// the merge that caused it rather than at it, so this yields rather than
// contests. Declared as aimee-postgres in src/modules/process-contracts.json.
const storePrincipalRef uint32 = 69

// aimeeDirectoryPrincipalRef is the aimee module's OUTBOUND identity, used only
// to read the session directory out of db1. Same reason as the economizer's: a
// serving grant requests nothing, so reaching another module's stage needs a
// second principal granted exactly that request.
//
// 67, and deliberately not the 69 the validation probe uses. Two clients sharing
// a ref are a duplicate principal and the bus refuses whichever attaches second,
// which would surface as a failure in whichever of the two started later rather
// than at the cause.
const aimeeDirectoryPrincipalRef uint32 = 67

// aimeeDirectory builds the peer capability's DirectorySource.
//
// The choice is EXPLICIT and defaults to none. AIMEE_PEER_DIRECTORY=db1 reads
// existence from db1's session family over the bus; anything else, including
// unset, declares that there is no directory and the session-scoped stages
// answer no_directory.
//
// Defaulting to none is not caution, it is accuracy. db1's server_session_get
// returns 0 only on SQLITE_ROW and -1 for everything else including no row, and
// its stage maps a non-zero rc to FAILED, so on the store that ships today an
// absent session and a broken store are one status. Under that contract this
// module would have to report either "gone" for a transient outage -- which
// destroys mail under the undeliverable rule -- or "retry" for a session that
// will never exist. The Go store distinguishes them and the catalog now declares
// all four, so this becomes the default when db1 runs it.
func aimeeDirectory(ctx context.Context, moduleBusSocket string) (aimee.DirectorySource, string) {
	if os.Getenv("AIMEE_PEER_DIRECTORY") != "db1" {
		return aimee.NoDirectory{}, "none (set AIMEE_PEER_DIRECTORY=db1 to read db1's session family)"
	}
	if ctx == nil || moduleBusSocket == "" {
		return aimee.NoDirectory{}, "none: db1 was asked for but there is no module bus socket"
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, aimeeDirectoryPrincipalRef)
	if err != nil {
		// Reported, not silently downgraded: a module that was told to use db1
		// and quietly did not would answer no_directory for a reason nobody
		// could see.
		return aimee.NoDirectory{}, fmt.Sprintf("none: could not attach as principal %d: %v",
			aimeeDirectoryPrincipalRef, err)
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return aimee.NoDirectory{}, fmt.Sprintf("none: no module caller: %v", err)
	}
	directory, err := aimee.NewDB1Directory(caller, 5*time.Second)
	if err != nil {
		// CloseAndWait BEFORE Detach. The caller's goroutine is polling the
		// shared-memory region by now and Detach unmaps it; the Detach above is
		// safe only because the constructor failed and no goroutine exists yet.
		// Same defect that segfaulted the probe after every check passed.
		caller.CloseAndWait()
		busClient.Detach()
		return aimee.NoDirectory{}, fmt.Sprintf("none: %v", err)
	}
	return directory, fmt.Sprintf("db1 sessions (kind %d) as principal 1/%d",
		peerwire.EventKind(aimee.DB1PrincipalRef, aimee.DB1SessionsStage), aimeeDirectoryPrincipalRef)
}

// economizerStore seats the economizer's reducer state on DB1.
//
// A failure here is not fatal to the module. Reducer state makes the NEXT turn
// cheaper, so a module that cannot reach the store still reduces -- it just
// never warms up. Refusing to serve would turn a lost optimization into a lost
// feature, so this returns a nil store and lets the caller carry on.
func economizerStore(ctx context.Context, moduleBusSocket string) economizer.StateStore {
	if ctx == nil || moduleBusSocket == "" {
		return nil
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, economizerStorePrincipalRef)
	if err != nil {
		return nil
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return nil
	}
	store, err := db1.NewClient(caller, 0)
	if err != nil {
		// CloseAndWait before Detach: the poll goroutine is live here and
		// Detach unmaps the region it reads.
		caller.CloseAndWait()
		busClient.Detach()
		return nil
	}
	return store
}

// storeBackend is db1's storage: the postgres module, over the bus.
//
// db1 opens no database. The postgres module owns the connection, the DSN and
// the pooling policy, and this is the client that asks it -- the same shape as
// economizerStore above, under db1's outbound identity.
// applySchemaWaiting applies the schema, giving the postgres module time to
// finish attaching first.
//
// THE SUPERVISOR GIVES NO ORDERING GUARANTEE. It starts every module in its
// manifest back to back and each registers its stages asynchronously, so the
// store routinely makes its first call before postgres has claimed kind 11266.
// Without this the store exits at startup and the daemon comes up storeless:
//
//	[module-supervisor:server] starting postgres
//	store: schema: read the applied schema version: ... capability absent
//	aimee-module: module "aimee" could not start
//
// Observed on a sixteen-module fleet, where the two start one line apart.
// Ordering the manifest would not fix it -- registration is asynchronous, so
// starting postgres first only narrows the window, and a window that closes on
// a fast machine reopens on a loaded one.
//
// A BOUNDED WAIT. If postgres is genuinely absent -- not installed, refused by
// its grant, unable to open the database -- the store must still fail and say
// so rather than hang forever looking healthy. The ceiling is what separates
// "not up yet" from "not coming".
//
// Retrying ONLY ErrStoreUnavailable, which is the transport reporting it could
// not reach the module at all. A store that answers and refuses is a different
// thing, and retrying that would turn one clear error into the same error
// thirty times.
func applySchemaWaiting(ctx context.Context, db aimee.Store) error {
	const (
		attempts = 30
		gap      = time.Second
	)
	var err error
	for attempt := 1; attempt <= attempts; attempt++ {
		err = families.ApplySchema(ctx, db)
		if err == nil {
			if attempt > 1 {
				log.Printf("store: the postgres module answered after %ds", attempt-1)
			}
			return nil
		}
		if !errors.Is(err, aimee.ErrStoreUnavailable) {
			return err
		}
		if attempt == attempts {
			break
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(gap):
		}
	}
	return fmt.Errorf("the postgres module did not answer within %ds: %w", attempts, err)
}

func storeBackend(ctx context.Context, moduleBusSocket string) (aimee.Store, error) {
	if ctx == nil || moduleBusSocket == "" {
		return nil, errors.New("store: no module bus to reach the postgres module on")
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, storePrincipalRef)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return nil, err
	}
	db, err := aimee.NewStore(caller)
	if err != nil {
		caller.CloseAndWait()
		busClient.Detach()
		return nil, err
	}
	return db, nil
}

func roundtableReviewer(ctx context.Context, moduleBusSocket string) (*roundtable.PanelReviewer, error) {
	home := os.Getenv("AIMEE_HOME")
	if home == "" {
		return nil, errors.New("AIMEE_HOME is unset")
	}
	if ctx == nil || moduleBusSocket == "" {
		return nil, errors.New("no module bus is configured")
	}
	presets, err := panel.NewStore(filepath.Join(home, "roundtables"))
	if err != nil {
		return nil, err
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, roundtableDelegatePrincipalRef)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		return nil, err
	}
	client, err := delegatecontract.NewBusClient(caller, 0)
	if err != nil {
		// CloseAndWait before Detach: the poll goroutine is live here and
		// Detach unmaps the region it reads.
		caller.CloseAndWait()
		busClient.Detach()
		return nil, err
	}
	return roundtable.NewPanelReviewer(presets, roundtable.NewBusDelegates(client))
}

// sandboxHome resolves the learned store's root the same way the WFE resolves
// its own: AIMEE_HOME, else the per-user config directory.
func sandboxHome() string {
	if home := os.Getenv("AIMEE_HOME"); home != "" {
		return home
	}
	userHome, err := os.UserHomeDir()
	if err != nil {
		return os.TempDir()
	}
	return filepath.Join(userHome, ".config", "aimee")
}

func moduleConfig(executable string) (bus.ModuleProcessConfig, bool) {
	return moduleConfigRuntime(nil, executable, "")
}

func moduleConfigRuntime(ctx context.Context, executable, moduleBusSocket string) (bus.ModuleProcessConfig, bool) {
	name := strings.TrimPrefix(filepath.Base(executable), "aimee-module-")
	config := bus.ModuleProcessConfig{PrincipalClass: 1}

	// Plugin modules are instanced: `aimee-module-mcp-github` hosts exactly one
	// MCP server under the group "github". They are matched by prefix because
	// the set is a deployment decision, not a compile-time list -- a fleet may
	// run ten of them, each its own process and its own failure domain.
	if instance, isPlugin := strings.CutPrefix(name, "mcp-"); isPlugin {
		return mcpModuleConfig(ctx, config, name, instance)
	}

	switch name {
	case "memory":
		config.ModuleName = name
		config.PrincipalRef = 7
		config.Stages = []bus.ModuleStage{
			{EventKind: memory.EventExtractIndex, StageID: memory.StageExtractIndex},
			{EventKind: memory.EventWrite, StageID: memory.StageWrite},
			{EventKind: memory.EventEmbed, StageID: memory.StageEmbed},
			{EventKind: memory.EventRetrieve, StageID: memory.StageRetrieve},
			{EventKind: memory.EventRerank, StageID: memory.StageRerank},
			{EventKind: memory.EventDeclareCommands, StageID: memory.StageDeclareCommands},
		}
		config.Handler = memory.Handle
	case "learning":
		config.ModuleName = name
		config.PrincipalRef = 8
		config.Stages = []bus.ModuleStage{{EventKind: learning.EventKind, StageID: learning.StageObserve}}
		config.Handler = learning.Handle
	case "routing":
		config.ModuleName = name
		config.PrincipalRef = 9
		config.Stages = []bus.ModuleStage{{EventKind: routing.EventKind, StageID: routing.StageSelect}}
		config.Handler = routing.Handle
	case "delegates":
		config.ModuleName = name
		config.PrincipalRef = 10
		config.Stages = []bus.ModuleStage{
			{EventKind: delegates.EventKind, StageID: delegates.StageInvoke},
			{EventKind: delegates.EventCapabilities, StageID: delegates.StageCapabilities},
			{EventKind: delegates.EventChain, StageID: delegates.StageChain},
			{EventKind: delegates.EventPaths, StageID: delegates.StagePaths},
			{EventKind: delegates.EventHandoff, StageID: delegates.StageHandoff},
			{EventKind: delegates.EventRescue, StageID: delegates.StageRescue},
			{EventKind: delegates.EventVerify, StageID: delegates.StageVerify},
			{EventKind: delegates.EventEconomics, StageID: delegates.StageEconomics},
			{EventKind: delegates.EventPatchCoord, StageID: delegates.StagePatchCoord},
			{EventKind: delegates.EventRolePolicy, StageID: delegates.StageRolePolicy},
			{EventKind: delegates.EventWorktreePlan, StageID: delegates.StageWorktreePlan},
			{EventKind: delegates.EventLaunchArgs, StageID: delegates.StageLaunchArgs},
			{EventKind: delegates.EventImageSpec, StageID: delegates.StageImageSpec},
			{EventKind: delegates.EventIsolation, StageID: delegates.StageIsolation},
			{EventKind: delegates.EventPermissions, StageID: delegates.StagePermissions},
			{EventKind: delegates.EventImageGC, StageID: delegates.StageImageGC},
			{EventKind: delegates.EventRouteFilter, StageID: delegates.StageRouteFilter},
			{EventKind: delegates.EventNoopWrite, StageID: delegates.StageNoopWrite},
			{EventKind: delegates.EventLaunchPlan, StageID: delegates.StageLaunchPlan},
			{EventKind: delegates.EventReviewEvidence, StageID: delegates.StageReviewEvidence},
			{EventKind: delegates.EventNamedFileDrift, StageID: delegates.StageNamedFileDrift},
			{EventKind: delegates.EventGroupPlan, StageID: delegates.StageGroupPlan},
		}
		var executor delegates.Executor
		registryExecutor, err := delegates.NewRegistryExecutor(os.Getenv("AIMEE_HOME"))
		if err != nil {
			log.Printf("delegate execution unavailable: %v", err)
		} else {
			executor = registryExecutor
		}
		config.Handler = delegates.NewHandler(executor)
	case "tools":
		config.ModuleName = name
		config.PrincipalRef = 11
		config.Stages = []bus.ModuleStage{{EventKind: moduletools.EventKind, StageID: moduletools.StageDispatch}}
		config.Handler = moduletools.Handle
	case "workspace":
		config.ModuleName = name
		config.PrincipalRef = 12
		config.Stages = []bus.ModuleStage{
			{EventKind: workspace.EventKind, StageID: workspace.StageAccess},
			{EventKind: workspace.EventRunner, StageID: workspace.StageRunner},
			{EventKind: workspace.EventRunnerIO, StageID: workspace.StageRunnerIO},
		}
		config.Handler = workspace.Handle
	case "git":
		config.ModuleName = name
		config.PrincipalRef = 13
		config.Stages = []bus.ModuleStage{
			{EventKind: modulegit.EventKind, StageID: modulegit.StageOperation},
			{EventKind: modulegit.EventRefValidate, StageID: modulegit.StageRefValidate},
			{EventKind: modulegit.EventCIGrade, StageID: modulegit.StageCIGrade},
			{EventKind: modulegit.EventForgeRequest, StageID: modulegit.StageForgeRequest},
			{EventKind: modulegit.EventCredResolve, StageID: modulegit.StageCredResolve},
			{EventKind: modulegit.EventVerifyRun, StageID: modulegit.StageVerifyRun},
		}
		config.Handler = modulegit.Handle
	case "skills":
		config.ModuleName = name
		config.PrincipalRef = 14
		config.Stages = []bus.ModuleStage{
			{EventKind: skills.EventKind, StageID: skills.StageContext},
			{EventKind: skills.EventTrigger, StageID: skills.StageTrigger},
		}
		config.Handler = skills.Handle
	case "response-composition":
		config.ModuleName = name
		config.PrincipalRef = 15
		config.Stages = []bus.ModuleStage{{EventKind: responsecomposition.EventKind, StageID: responsecomposition.StageCompose}}
		config.Handler = responsecomposition.Handle
	case "governance":
		config.ModuleName = name
		config.PrincipalRef = 19
		config.Stages = []bus.ModuleStage{{EventKind: governance.EventEvaluate, StageID: governance.StageEvaluate}}
		config.Handler = governance.Handle
	case "roundtable":
		config.ModuleName = name
		config.PrincipalRef = 21
		// Deliberate and chunk planning need nothing but their arguments, so both
		// are declared unconditionally. Review is appended below only when this
		// process can actually convene one.
		config.Stages = []bus.ModuleStage{
			{EventKind: roundtable.EventDeliberate, StageID: roundtable.StageDeliberate},
			{EventKind: roundtable.EventChunkPlan, StageID: roundtable.StageChunkPlan},
		}
		config.Handler = roundtable.Handle
		// Deliberate is a pure rubric and always available. Review convenes real
		// agents, so it is served only when this process can actually reach the
		// delegates module and the saved roundtables. Declaring the stage anyway
		// would make an unreachable review look like a failing one; leaving it
		// undeclared makes the daemon report the module as not serving that kind,
		// which is what is true.
		if reviewer, err := roundtableReviewer(ctx, moduleBusSocket); err != nil {
			log.Printf("roundtable review stage unavailable: %v", err)
		} else {
			config.Stages = append(config.Stages,
				bus.ModuleStage{EventKind: roundtable.EventReview, StageID: roundtable.StageReview})
			config.Handler = roundtable.NewHandler(reviewer)
		}
	case "kb-synthesis":
		config.ModuleName = name
		config.PrincipalRef = 22
		config.Stages = []bus.ModuleStage{{EventKind: kbsynthesis.EventGrounding, StageID: kbsynthesis.StageGrounding}}
		config.Handler = kbsynthesis.Handle
	case "runtime-web":
		config.ModuleName = name
		config.PrincipalRef = 23
		config.Stages = []bus.ModuleStage{{EventKind: runtimeweb.EventClassify, StageID: runtimeweb.StageClassify}}
		config.Handler = runtimeweb.Handle
	case "control-web":
		config.ModuleName = name
		config.PrincipalRef = 24
		config.Stages = []bus.ModuleStage{{EventKind: controlweb.EventAuthorize, StageID: controlweb.StageAuthorize}}
		config.Handler = controlweb.Handle
	case "sandbox":
		config.ModuleName = name
		config.PrincipalRef = 26
		config.Stages = []bus.ModuleStage{
			{EventKind: sandbox.EventObserve, StageID: sandbox.StageObserve},
			{EventKind: sandbox.EventLoad, StageID: sandbox.StageLoad},
			{EventKind: sandbox.EventProxyRequest, StageID: sandbox.StageProxyRequest},
			{EventKind: sandbox.EventProxyAddress, StageID: sandbox.StageProxyAddress},
		}
		// The learned store lives under AIMEE_HOME. Unlike roundtable's review
		// stage -- which needs a resource plane that may genuinely be absent --
		// a home always resolves, so both stages are unconditional and the same
		// fallback the WFE uses applies here.
		store, err := sandbox.NewStore(sandboxHome())
		if err != nil {
			log.Printf("sandbox stages unavailable: %v", err)
			return bus.ModuleProcessConfig{}, false
		}
		config.Handler = sandbox.NewHandler(store)
	case "economizer":
		config.ModuleName = name
		config.PrincipalRef = 27
		config.Stages = []bus.ModuleStage{
			{EventKind: economizer.EventReduce, StageID: economizer.StageReduce},
			{EventKind: economizer.EventJSONCompact, StageID: economizer.StageJSONCompact},
			{EventKind: economizer.EventToolRecall, StageID: economizer.StageToolRecall},
			{EventKind: economizer.EventToolStats, StageID: economizer.StageToolStats},
			{EventKind: economizer.EventRecordBuild, StageID: economizer.StageRecordBuild},
			{EventKind: economizer.EventPostStatus, StageID: economizer.StagePostStatus},
			{EventKind: economizer.EventStats, StageID: economizer.StageStats},
		}
		// Per-conversation reducer state is the module's own, kept in DB1 over
		// the bus. An unreachable store is not a failure mode before serving:
		// the module reduces without warming up.
		config.Handler = economizer.NewHandlerWithStore(
			economizerStore(ctx, moduleBusSocket))
	case "postgres":
		config.ModuleName = name
		config.PrincipalRef = 28
		// Two stages, and the SQL one is what every store call in the tree
		// ultimately lands on: aimee keeps no database and reaches PostgreSQL
		// through here. Before it existed the store module attached, built its
		// client, found nothing serving 11266 and exited -- which read as "the
		// store is absent" on a system where every other part had been written.
		config.Stages = []bus.ModuleStage{
			{EventKind: postgres.EventHealth, StageID: postgres.StageHealth},
			{EventKind: postgres.EventSQL, StageID: postgres.StageSQL},
		}
		// BOTH STAGES, ALWAYS. The SQL handler opens its pool on first use and
		// answers with the reason when it cannot, so a missing DSN produces an
		// explained refusal rather than a stage that is declared and absent.
		// Trimming the list here instead would make this process disagree with
		// process-contracts.json exactly when the database is unreachable.
		sqlHandler := postgres.NewSQLHandler()
		config.Handler = func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
			if invocation.StageID == postgres.StageSQL {
				return sqlHandler(invocation, frame)
			}
			return postgres.Handle(invocation, frame)
		}
	// "aimee" is what a deployment installs this as: the id in
	// process-contracts.json and the executable every generated grant pins.
	//
	// "db1" and "store" stay accepted rather than being cleaned away. A grant
	// generated before the rename pins the old name, and an installed deployment
	// does not regenerate its grants because this tree moved a directory -- so
	// dropping them turns an upgrade into a module that cannot attach, which
	// presents as the store being absent rather than as a name that changed.
	case "aimee", "store", "db1":
		// The module HOSTS CAPABILITIES. Two of them meet here: the store --
		// every table the daemon keeps, on one PostgreSQL database -- and peer
		// messaging, which was its own module until this principal absorbed it.
		// A third is another argument to New, not a restructure.
		//
		// The store is a different principal from the "postgres" health probe
		// below, deliberately: the probe keeps its own connection precisely so
		// it can still answer when this module's pool is broken, and a probe
		// sharing the pool it reports on would read healthy right up until it
		// could not answer at all.
		config.ModuleName = name
		config.PrincipalRef = aimee.PrincipalRef

		// Peer messaging. The registry is process-local: inboxes and grants
		// live in memory and do not survive a bounce.
		//
		// Its DirectorySource is the store's session family, which is why the
		// store is REQUIRED below rather than optional. Peer messaging with no
		// directory is not degraded, it is inert -- every session-scoped call
		// refuses with unknown_sender or no_peer, which are answers ABOUT A
		// SESSION from a module that cannot know about any session. Serving
		// four peer stages out of this principal's twenty-three while the store
		// is unreachable would advertise exactly that: a correct-looking
		// refusal from something that can never do anything.
		directory, sourceDescription := aimeeDirectory(ctx, moduleBusSocket)
		peerCapability, err := aimee.NewPeer(peer.New(peer.Options{}), directory)
		if err != nil {
			log.Printf("aimee module unavailable: %v", err)
			return bus.ModuleProcessConfig{}, false
		}
		// Logged at every start, not once at build time: an operator reading
		// why a peer send refuses should find the reason in the log of the
		// process that refused it. It names the source either way, so a run
		// that MEANT to use the session family and did not is visible rather
		// than looking the same as one that never asked.
		log.Printf("aimee module: session directory = %s", sourceDescription)

		db, err := storeBackend(ctx, moduleBusSocket)
		if err != nil {
			// Without a store this module serves nothing. Declaring its stages
			// anyway would have the daemon route every store call here to fail
			// one at a time; declaring none makes it report the kinds as
			// unserved, which is what is true.
			log.Printf("store: no store backend: %v", err)
			return config, false
		}
		// Create anything missing before serving. Nothing else applies this
		// schema -- there is no deploy step for it -- so a fresh database would
		// otherwise come up empty and fail every call against tables that were
		// never created.
		schemaCtx, cancelSchema := context.WithTimeout(context.Background(), 2*time.Minute)
		err = applySchemaWaiting(schemaCtx, db)
		cancelSchema()
		if err != nil {
			log.Printf("store: schema: %v", err)
			return config, false
		}
		log.Printf("store: schema applied (%d files)", families.SchemaFileCount())
		mux, err := aimee.NewMux(db, families.All()...)
		if err != nil {
			log.Printf("store: %v", err)
			return config, false
		}
		for _, bind := range families.Binds(db) {
			if err := mux.Add(bind); err != nil {
				log.Printf("store: %v", err)
				return config, false
			}
		}
		// One stage table, one handler. Mux answers stages 1..19 and the peer
		// capability 20..23; New refuses a collision at construction rather
		// than letting the loser be silently unreachable.
		module, err := aimee.New(peerCapability, mux)
		if err != nil {
			log.Printf("aimee module unavailable: %v", err)
			return bus.ModuleProcessConfig{}, false
		}
		config.Stages = module.Stages()
		config.Handler = module.Handle
	case "benchmarks":
		config.ModuleName = name
		config.PrincipalRef = 25
		config.Stages = []bus.ModuleStage{
			{EventKind: benchmarks.EventRun, StageID: benchmarks.StageRun},
			{EventKind: benchmarks.EventLatency, StageID: benchmarks.StageLatency},
		}
		config.Handler = benchmarks.Handle
	default:
		return bus.ModuleProcessConfig{}, false
	}
	return config, true
}

// mcpModuleConfig builds the process config for one MCP plugin instance.
//
// PRINCIPAL REF IS NOT ALLOCATED HERE, DELIBERATELY. Every other module carries
// a compile-time constant (7..28), which cannot work for a set whose membership
// is a deployment decision. Inventing an allocation -- hashing the instance name
// into a band, say -- would be inventing an AUTHORIZATION policy, and a
// collision there means two plugins sharing one grant. So the ref is supplied by
// whatever provisions the instance, and a module with no ref refuses to start
// rather than defaulting to something. Fail closed; the allocation scheme is its
// own decision.
//
// The plugin itself is NOT started here. Attaching a plugin means running
// third-party code, and the supply-chain gate that covers aimee.yaml-declared
// MCP clients (the OSV scan in src/cmd_mcp.c, plus permission and egress
// admission) does not yet cover module-hosted ones. Until it does, this module
// serves its stages with no plugin attached: it declares zero commands and
// answers CapabilityAbsent. That is a working, inert module -- not a gap left
// open.
func mcpModuleConfig(ctx context.Context, config bus.ModuleProcessConfig, name, instance string) (bus.ModuleProcessConfig, bool) {
	if instance == "" {
		log.Printf("mcp module: executable names no instance (want aimee-module-mcp-NAME)")
		return bus.ModuleProcessConfig{}, false
	}
	// The permission ceiling is what this instance may do at most. Unset means
	// `read` -- the least privilege -- matching plugin_permission_from_str().
	ceiling := mcpmodule.ParsePermission(os.Getenv("AIMEE_MCP_PLUGIN_PERMISSION"))
	module := mcpmodule.New(instance, ceiling)
	if module.Group() == "" {
		log.Printf("mcp module: instance %q has no usable command group", instance)
		return bus.ModuleProcessConfig{}, false
	}
	ref, err := principalRefFromEnv()
	if err != nil {
		log.Printf("mcp module %s: %v", name, err)
		return bus.ModuleProcessConfig{}, false
	}
	// The kinds come from the ref, not from a second environment variable. An
	// independently supplied base is a second allocation authority for one
	// namespace, which is exactly how the old range came to squat postgres's
	// kinds; see the derivation comment in modules/mcp.
	invoke, declare, err := mcpmodule.EventKinds(ref)
	if err != nil {
		log.Printf("mcp module %s: %v", name, err)
		return bus.ModuleProcessConfig{}, false
	}
	if err := checkLegacyEventBase(invoke); err != nil {
		log.Printf("mcp module %s: %v", name, err)
		return bus.ModuleProcessConfig{}, false
	}

	config.ModuleName = name
	config.PrincipalRef = ref
	config.Stages = []bus.ModuleStage{
		{EventKind: invoke, StageID: mcpmodule.StageInvoke},
		{EventKind: declare, StageID: mcpmodule.StageDeclareCommands},
	}
	config.Handler = module.Handle

	// The plugin is RECORDED, not started.
	//
	// Starting it executes third-party code, so it waits for the daemon's
	// admission verdict -- the same OSV malware gate that has always guarded an
	// aimee.yaml-declared MCP server (mcp_osv_gate.c, shared by both paths).
	// With no argv the module runs inert: it serves its stages, declares zero
	// commands, and answers CapabilityAbsent.
	if argv := pluginArgvFromEnv(); len(argv) > 0 {
		module.SetPending(argv, os.Getenv("AIMEE_MCP_PLUGIN_CWD"), ceiling)
		log.Printf("%s: plugin recorded, awaiting admission (ceiling %s)", name, ceiling)
	}
	if ctx != nil {
		// Reap the plugin on shutdown rather than orphaning it.
		go func() {
			<-ctx.Done()
			module.Detach()
		}()
	}
	return config, true
}

// pluginArgvFromEnv reads the plugin command line as a JSON array.
//
// JSON rather than a shell string so an argument containing a space is exact:
// splitting on whitespace is how a path with a space becomes two broken
// arguments, and the failure shows up as "plugin did not start" with no clue.
func pluginArgvFromEnv() []string {
	raw := os.Getenv("AIMEE_MCP_PLUGIN_ARGV")
	if raw == "" {
		return nil
	}
	var argv []string
	if err := json.Unmarshal([]byte(raw), &argv); err != nil {
		log.Printf("AIMEE_MCP_PLUGIN_ARGV is not a JSON array of strings: %v", err)
		return nil
	}
	if len(argv) == 0 || argv[0] == "" {
		log.Printf("AIMEE_MCP_PLUGIN_ARGV names no executable")
		return nil
	}
	return argv
}

// checkLegacyEventBase rejects an instance still carrying the retired
// AIMEE_MODULE_EVENT_BASE variable when it disagrees with the ref-derived kinds.
//
// Kinds are now derived from the principal ref, so the variable is obsolete. It
// is not merely ignored: a deployment provisioned under the old scheme has a
// .grant whose `serve=` list names the OLD kinds, and those kinds sit in the
// blocks belonging to postgres, db2 and db1. Starting such an instance would
// either be denied at attach or, worse, win the race and deny a core module.
// Failing here with a pointer to re-provisioning is the safe outcome.
func checkLegacyEventBase(invoke uint32) error {
	raw := os.Getenv("AIMEE_MODULE_EVENT_BASE")
	if raw == "" {
		return nil
	}
	base, err := strconv.ParseUint(raw, 10, 32)
	if err != nil {
		return fmt.Errorf("AIMEE_MODULE_EVENT_BASE=%q is not a 32-bit id", raw)
	}
	if uint32(base) == invoke {
		return nil // agrees with the derivation; harmless leftover
	}
	return fmt.Errorf("AIMEE_MODULE_EVENT_BASE=%d is stale: event kinds are now derived "+
		"from the principal ref (this instance's invoke kind is %d). Re-run "+
		"scripts/provision-plugin-module.py for this instance to rewrite its .grant",
		base, invoke)
}

// principalRefFromEnv reads the instance's provisioned principal reference.
func principalRefFromEnv() (uint32, error) {
	raw := os.Getenv("AIMEE_MODULE_PRINCIPAL_REF")
	if raw == "" {
		return 0, errors.New("AIMEE_MODULE_PRINCIPAL_REF is not set; an instanced module " +
			"cannot allocate its own authorization identity")
	}
	ref, err := strconv.ParseUint(raw, 10, 32)
	if err != nil || ref == 0 {
		return 0, fmt.Errorf("AIMEE_MODULE_PRINCIPAL_REF=%q is not a positive 32-bit id", raw)
	}
	return uint32(ref), nil
}

func run(ctx context.Context, args []string) error {
	if len(args) != 2 {
		return errUsage
	}
	config, ok := moduleConfigRuntime(ctx, args[0], args[1])
	if !ok {
		// A recognised module that could not start sets its name before giving
		// up, and has already logged why. Reporting that as an unknown
		// executable sends the reader to check the binary's name when the real
		// answer -- a missing DSN, an unreachable database -- is the line above.
		if config.ModuleName != "" {
			return fmt.Errorf("module %q could not start; see the error above",
				config.ModuleName)
		}
		return fmt.Errorf("unknown Go module executable %q", filepath.Base(args[0]))
	}
	if config.ModuleName == "postgres" {
		defer postgres.Close()
	}
	config.SocketPath = args[1]
	return bus.RunModuleProcess(ctx, config)
}

func main() {
	if handled, code := delegates.RunWatchdog(os.Args); handled {
		os.Exit(code)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args); err != nil {
		fmt.Fprintf(os.Stderr, "aimee-module: %v\n", err)
		os.Exit(1)
	}
}
