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
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/modules/aimee"
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
		busClient.Detach()
		return nil
	}
	return store
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
	case "aimee":
		config.ModuleName = name
		config.PrincipalRef = aimee.PrincipalRef
		// The module hosts capabilities; peer messaging is the first. A second
		// capability is another argument here, not a restructure.
		//
		// The registry is process-local today: inboxes and grants live in
		// memory and do not survive a bounce. Durable storage arrives through
		// the postgres module's generic wire and changes nothing here.
		//
		// NoDirectory{} is not a placeholder, it is the accurate description of
		// this build. There is no DirectorySource yet -- it needs db1's session
		// family, which lands with the absorption -- and nothing else populates
		// the registry either: Register has no caller outside tests, and no bus
		// op reaches it. So no session can exist here, and PEER MESSAGING IS
		// INERT IN THIS CONFIGURATION.
		//
		// Said explicitly because the previous nil said it silently. Every
		// session-scoped call refused with unknown_sender or no_peer, which are
		// answers ABOUT A SESSION from a module that could not know about any
		// session, and every refusal check in the container validation passed
		// against exactly this state. A correct refusal and a module that can
		// never do anything produce the same word.
		directory, sourceDescription := aimeeDirectory(ctx, moduleBusSocket)
		peerCapability, err := aimee.NewPeer(peer.New(peer.Options{}), directory)
		if err != nil {
			log.Printf("aimee module unavailable: %v", err)
			return bus.ModuleProcessConfig{}, false
		}
		// Logged at every start, not once at build time: an operator reading
		// why a peer send refuses should find the reason in the log of the
		// process that refused it. It names the source either way, so a run
		// that MEANT to use db1 and did not is visible rather than looking the
		// same as one that never asked.
		log.Printf("aimee module: session directory = %s", sourceDescription)
		module, err := aimee.New(peerCapability)
		if err != nil {
			// A stage conflict is a programming error in the capability list,
			// not a runtime condition: refusing to advertise is better than
			// advertising a stage served by the wrong owner.
			log.Printf("aimee module unavailable: %v", err)
			return bus.ModuleProcessConfig{}, false
		}
		config.Stages = module.Stages()
		config.Handler = module.Handle
	case "postgres":
		config.ModuleName = name
		config.PrincipalRef = 28
		config.Stages = []bus.ModuleStage{{EventKind: postgres.EventHealth, StageID: postgres.StageHealth}}
		config.Handler = postgres.Handle
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
