// Command aimee-module is a multicall executable for isolated Go module processes.
package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/db1"
	db2contract "github.com/JBailes/aimee/server-go/db2"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/modules/benchmarks"
	controlplane "github.com/JBailes/aimee/server-go/modules/control-plane"
	controlweb "github.com/JBailes/aimee/server-go/modules/control-web"
	"github.com/JBailes/aimee/server-go/modules/db2"
	"github.com/JBailes/aimee/server-go/modules/delegates"
	"github.com/JBailes/aimee/server-go/modules/economizer"
	modulegit "github.com/JBailes/aimee/server-go/modules/git"
	"github.com/JBailes/aimee/server-go/modules/governance"
	kbsynthesis "github.com/JBailes/aimee/server-go/modules/kb-synthesis"
	"github.com/JBailes/aimee/server-go/modules/learning"
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
	storage "github.com/JBailes/aimee/server-go/postgres"
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

// controlPlaneStorePrincipalRef is the control-plane module's OUTBOUND identity,
// for the same reason: its serving grant requests nothing, so reaching the
// postgres module's storage stage needs a second principal granted exactly that
// one request.
const controlPlaneStorePrincipalRef uint32 = 69

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
		busClient.Detach()
		return nil
	}
	return store
}

// controlPlaneStore seats the control-plane module's storage on the postgres
// module, and applies its schema.
//
// THE FIRST THING IN THIS TREE THAT REACHES POSTGRESQL THROUGH THE MODULE IN A
// RUNNING DEPLOYMENT. The storage stage was built, registered, declared for
// both placements and proven at parity, and nothing consumed it: the deployed
// aimee-module-db2 is the C build, and the Go db2 module that does reach
// PostgreSQL this way is not the deployed db2. So the architecture was true of a
// test path and false of the product.
//
// A failure here is not fatal. postgres is in the KB's optional module list, so
// an operator can turn it off; the module then reports storage unreachable and
// keeps serving health, which is the truthful answer. Refusing to start would
// turn an operator's choice into an outage.
func controlPlaneStore(ctx context.Context, moduleBusSocket string) {
	if ctx == nil || moduleBusSocket == "" {
		return
	}
	busClient, err := bus.ConnectClient(ctx, moduleBusSocket, 1, controlPlaneStorePrincipalRef)
	if err != nil {
		log.Printf("control-plane: no storage bus (%v); serving health without it", err)
		return
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, busClient)
	if err != nil {
		busClient.Detach()
		log.Printf("control-plane: no storage caller (%v); serving health without it", err)
		return
	}
	client := storage.New(func(callCtx context.Context, body []byte) ([]byte, error) {
		return caller.Call(callCtx, postgres.EventSQL, postgres.StageSQL, 0,
			controlPlaneStoreDeadline, body)
	})
	controlplane.UseStore(client)

	// The schema, and a row proving the round trip, on the way up. Logged rather
	// than fatal for the reason above -- and logged on success too, because
	// "storage is reachable" is the fact this commit exists to make true and an
	// operator should be able to see it in the journal rather than infer it.
	startCtx, cancel := context.WithTimeout(ctx, controlPlaneStoreDeadline)
	defer cancel()
	if err := controlplane.RecordStart(startCtx, time.Now().UTC().Format(time.RFC3339),
		aimeeVersion()); err != nil {
		log.Printf("control-plane: storage unreachable at start (%v); health will say so", err)
		return
	}
	log.Printf("control-plane: schema %s applied through the postgres module",
		controlplane.SchemaOwner)
}

// How long the control-plane module waits on a storage call. Generous compared
// with a health probe because a migration runs DDL, and mean compared with a
// startup that hangs: a module that never finishes starting is worse than one
// that reports storage unreachable and serves.
const controlPlaneStoreDeadline = 10 * time.Second

// aimeeVersion is what this build calls itself, for the recorded row.
func aimeeVersion() string {
	if version := os.Getenv("AIMEE_VERSION"); version != "" {
		return version
	}
	return "unknown"
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
	case "control-plane":
		config.ModuleName = name
		config.PrincipalRef = 32
		config.Stages = []bus.ModuleStage{
			{EventKind: controlplane.EventHealth, StageID: controlplane.StageHealth},
		}
		// Storage first, so the schema is applied before anything is served and
		// the first health call answers from a database rather than from a
		// not-yet-tried one.
		controlPlaneStore(ctx, moduleBusSocket)
		config.Handler = controlplane.Handle
	case "postgres":
		config.ModuleName = name
		config.PrincipalRef = 28
		// Health answers whether the database is usable; storage is the
		// database itself, for whichever module owns the rows. Two stages
		// rather than two modules: they share one pool, and a health probe
		// that opened its own connection would be reporting on a pool nobody
		// serves from.
		config.Stages = []bus.ModuleStage{
			{EventKind: postgres.EventHealth, StageID: postgres.StageHealth},
			{EventKind: postgres.EventSQL, StageID: postgres.StageSQL},
		}
		storage := postgres.NewSQLHandler()
		config.Handler = func(invocation bus.ModuleInvocation, body []byte) (
			[]byte, bus.ModuleStatus,
		) {
			if invocation.StageID == postgres.StageSQL {
				return storage.Handle(invocation, body)
			}
			return postgres.Handle(invocation, body)
		}
	case "db2":
		config.ModuleName = name
		config.PrincipalRef = 29
		// One stage per family, which is what the wire calls a stage: the
		// dispatcher keys on the (stage, operation) pair, and the operation
		// travels in the envelope. Eight stages therefore serve all 445
		// catalogued operations.
		config.Stages = []bus.ModuleStage{
			{EventKind: db2contract.EventLifecycle, StageID: db2contract.FamilyLifecycle},
			{EventKind: db2contract.EventTenancy, StageID: db2contract.FamilyTenancy},
			{EventKind: db2contract.EventMemory, StageID: db2contract.FamilyMemory},
			{EventKind: db2contract.EventIndex, StageID: db2contract.FamilyIndex},
			{EventKind: db2contract.EventLearning, StageID: db2contract.FamilyLearning},
			{EventKind: db2contract.EventOrganization, StageID: db2contract.FamilyOrganization},
			{EventKind: db2contract.EventCustody, StageID: db2contract.FamilyCustody},
			{EventKind: db2contract.EventMaintenance, StageID: db2contract.FamilyMaintenance},
		}
		config.Handler = db2.NewLazyDispatchHandler()
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

func run(ctx context.Context, args []string) error {
	if len(args) != 2 {
		return errUsage
	}
	config, ok := moduleConfigRuntime(ctx, args[0], args[1])
	if !ok {
		return fmt.Errorf("unknown Go module executable %q", filepath.Base(args[0]))
	}
	if config.ModuleName == "postgres" {
		defer postgres.Close()
	}
	if config.ModuleName == "db2" {
		defer db2.CloseProductionStore()
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
