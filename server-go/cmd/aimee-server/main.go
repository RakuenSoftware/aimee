package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	appconfig "github.com/JBailes/aimee/server-go/config"
	db1contract "github.com/JBailes/aimee/server-go/db1"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
	"github.com/JBailes/aimee/server-go/internal/api"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/engine"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	"github.com/JBailes/aimee/server-go/modules/observability"
	roundtablemod "github.com/JBailes/aimee/server-go/modules/roundtable"
	"github.com/JBailes/aimee/server-go/modules/workflows"
)

func configuredForge(url, socket string) (engine.Forge, error) {
	if url == "" && socket == "" {
		return nil, nil
	}
	// Forge credentials remain behind the owner-only Unix resource plane. A URL
	// without its socket is invalid configuration, not a reason to silently run
	// the native workflow engine without forge support.
	forge, err := engine.NewHTTPForge(engine.HTTPForgeConfig{BaseURL: url, UnixSocket: socket})
	if err != nil {
		return nil, err
	}
	return forge, nil
}

func main() {
	homeDefault := os.Getenv("AIMEE_HOME")
	if homeDefault == "" {
		userHome, err := os.UserHomeDir()
		if err != nil {
			log.Fatal(err)
		}
		homeDefault = filepath.Join(userHome, ".config", "aimee")
	}
	home := flag.String("home", homeDefault, "aimee state directory")
	socket := flag.String("socket", "", "Unix socket path")
	runnerURL := flag.String("runner-url", os.Getenv("AIMEE_WFE_RUNNER_URL"),
		"typed WFE runner endpoint; empty keeps execution disabled")
	runnerSocket := flag.String("runner-socket", os.Getenv("AIMEE_WFE_RUNNER_SOCKET"),
		"optional Unix socket for the typed WFE runner")
	forgeURL := flag.String("forge-service-url", os.Getenv("AIMEE_FORGE_SERVICE_URL"),
		"legacy forge resource-plane base URL")
	forgeSocket := flag.String("forge-service-socket", os.Getenv("AIMEE_FORGE_SERVICE_SOCKET"),
		"legacy forge resource-plane Unix socket")
	workflowDir := flag.String("workflow-dir", "", "workflow definition directory")
	moduleBusSocket := flag.String("module-bus-socket", os.Getenv("AIMEE_MODULE_BUS_SOCKET"),
		"daemon module bus socket; reviews are requested over it")
	metricsConfig := observability.MetricsServerConfigFromEnv("AIMEE_SERVER")
	flag.StringVar(&metricsConfig.Endpoint, "observability-listen", metricsConfig.Endpoint,
		"optional Prometheus listener: tcp://host:port or unix:///absolute/path")
	flag.StringVar(&metricsConfig.TLSCertificateFile, "observability-tls-certificate",
		metricsConfig.TLSCertificateFile, "TLS certificate chain for the Prometheus listener")
	flag.StringVar(&metricsConfig.TLSKeyFile, "observability-tls-key",
		metricsConfig.TLSKeyFile, "owner-only TLS private key for the Prometheus listener")
	flag.StringVar(&metricsConfig.TLSClientCAFile, "observability-tls-client-ca",
		metricsConfig.TLSClientCAFile, "CA bundle requiring Prometheus client certificates")
	flag.StringVar(&metricsConfig.BearerTokenFile, "observability-bearer-token-file",
		metricsConfig.BearerTokenFile, "owner-only file containing the Prometheus bearer token")
	concurrency := flag.Int("workflow-concurrency", envInt("AIMEE_AUTONOMY_CONCURRENCY", 5),
		"maximum concurrent work items across the whole WFE (total agent budget)")
	flag.Parse()
	if *socket == "" {
		*socket = filepath.Join(*home, "aimee-server.sock")
	}
	if *workflowDir == "" {
		*workflowDir = filepath.Join(*home, "workflows")
	}
	if err := os.MkdirAll(*home, 0o700); err != nil {
		log.Fatalf("create aimee home: %v", err)
	}
	telemetry, err := observability.New(context.Background(),
		observability.ConfigFromEnv("aimee-server", os.Getenv("AIMEE_VERSION")))
	if err != nil {
		log.Fatalf("initialize observability: %v", err)
	}
	slog.SetDefault(telemetry.LocalLogger("aimee-server"))
	var metricsServer *observability.MetricsServer
	defer func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if metricsServer != nil {
			if err := metricsServer.Shutdown(ctx); err != nil {
				log.Printf("shutdown observability listener: %v", err)
			}
		}
		if err := telemetry.Shutdown(ctx); err != nil {
			log.Printf("shutdown observability: %v", err)
		}
	}()

	// The store is the DB1 module now, not a file. This process used to open
	// $home/aimee.db directly -- the module's own file -- which made two
	// processes with two schema authorities on one store. It reaches the module
	// over the bus instead, which is why the attach happens here, before
	// anything that needs a store, rather than further down beside the runner.
	rootCtx, rootCancel := context.WithCancel(context.Background())
	defer rootCancel()
	if *moduleBusSocket == "" {
		log.Fatal("the workflow engine needs --module-bus-socket " +
			"(or AIMEE_MODULE_BUS_SOCKET): DB1 is reached through the module, not a file")
	}
	// Wait rather than race the supervisor. The daemon owns the bus socket and
	// the DB1 module attaches to it on its own schedule, so a WFE that started
	// first would otherwise exit on a boot ordering it cannot control -- and a
	// crash-looping engine is a worse failure than a slow one.
	attached, err := attachWithRetry(rootCtx, *moduleBusSocket, 60*time.Second)
	if err != nil {
		log.Fatalf("attach to the module bus: %v", err)
	}
	caller, err := bus.NewConcurrentModuleCaller(rootCtx, attached)
	if err != nil {
		log.Fatalf("module bus caller: %v", err)
	}
	// Shutdown must cancel and drain callers before unmapping their shared bus
	// region. Detaching first races scheduler/config calls already inside emit
	// and can turn an ordinary SIGTERM into a use-after-unmap SIGSEGV.
	defer func() {
		rootCancel()
		caller.CloseAndWait()
		attached.Detach()
	}()
	storeClient, err := db1contract.NewClient(caller, 0)
	if err != nil {
		log.Fatalf("db1 bus client: %v", err)
	}
	store, err := db1.OpenBus(storeClient)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	// Attached is not the same as served: the module may still be starting. Wait
	// for it to answer before anything asks it a question it would report as a
	// failure rather than as a delay.
	if err := waitForStore(rootCtx, store, 60*time.Second); err != nil {
		log.Fatalf("DB1 module did not answer: %v", err)
	}
	configClient, err := appconfig.NewClient(caller, 0)
	if err != nil {
		log.Fatalf("config bus client: %v", err)
	}
	if err := waitForConfig(rootCtx, configClient, 60*time.Second); err != nil {
		log.Fatalf("config module did not answer: %v", err)
	}
	artifacts, err := wfe.NewArtifactStore(filepath.Join(*home, "wfe-artifacts"))
	if err != nil {
		log.Fatal(err)
	}
	handler, err := api.New(store, artifacts, *workflowDir)
	if err != nil {
		log.Fatal(err)
	}
	handler.SetConfigStore(configClient)
	// The WFE control plane is deliberately Unix-socket-only. Credentials must
	// never be carried in this long-lived process's argv or environment; the
	// socket's ownership and 0600 mode are the authentication boundary.
	server := &http.Server{
		Handler:           telemetry.HTTPHandler("aimee-server.http", handler),
		ReadHeaderTimeout: 15 * time.Second,
	}
	var runner engine.Runner
	var worktreeManager *engine.WorktreeManager
	if *runnerURL != "" {
		runner, err = engine.NewHTTPRunner(engine.HTTPRunnerConfig{
			Endpoint: *runnerURL, UnixSocket: *runnerSocket,
		})
		if err != nil {
			log.Fatal(err)
		}
	} else {
		// One attach, one caller: the store above already holds it, and a second
		// attach under the same principal is a second slot the bus has to reap.
		agents, clientErr := delegatecontract.NewBusClient(caller, 0)
		if clientErr != nil {
			log.Fatal(clientErr)
		}
		worktrees, worktreeErr := engine.NewWorktreeManager(store, filepath.Join(*home, "wfe-worktrees"))
		if worktreeErr != nil {
			log.Fatal(worktreeErr)
		}
		worktreeManager = worktrees
		workflowRegistry, registryErr := wfe.NewRegistry(*workflowDir)
		if registryErr != nil {
			log.Fatal(registryErr)
		}
		forge, forgeErr := configuredForge(*forgeURL, *forgeSocket)
		if forgeErr != nil {
			log.Fatal(forgeErr)
		}
		nativeRunner, runnerErr := engine.NewNativeRunner(store, worktrees, agents, nil, artifacts, workflowRegistry, forge)
		if runnerErr != nil {
			log.Fatal(runnerErr)
		}
		// Reviews run in the roundtable module over the daemon's bus. This process
		// attaches as a requesting principal under its generated grant; it does
		// not host a panel, so there is one implementation and one place that
		// spends money convening seats.
		//
		// A gate whose reviewer never attached parks with that reason rather than
		// failing the run, so a bus that is not up yet delays reviews instead of
		// losing work.
		if *moduleBusSocket != "" {
			reviewer, reviewerErr := engine.NewBusReviewer(rootCtx, *moduleBusSocket,
				engine.BusPrincipalClass, engine.WFEBusPrincipalRef, 0)
			if reviewerErr != nil {
				log.Printf("roundtable reviews unavailable: %v", reviewerErr)
			} else {
				// Say so on success too. A control plane that attached and one that
				// silently did not look identical from outside until a gate hangs
				// waiting for a reply that was never routed.
				//
				// Word it as the REQUESTER attaching, which is all this proves.
				// The previous text -- "roundtable reviews over the event bus" --
				// reads as "reviews are available", and it printed identically with
				// the roundtable module disabled, because attaching as a requester
				// does not depend on anyone serving. Diagnosing a run where the
				// module was deliberately off, that line was the single strongest
				// piece of evidence that it was actually on.
				log.Printf("roundtable review requests will be sent over the event bus "+
					"(socket=%s principal=%d/%d kind=%d); a roundtable module must be "+
					"attached to answer them",
					*moduleBusSocket, engine.BusPrincipalClass, engine.WFEBusPrincipalRef,
					roundtablemod.EventReview)
				nativeRunner.SetRoundtableReviewer(reviewer)
			}
		} else {
			log.Printf("roundtable reviews unavailable: no module bus socket configured")
		}
		runner = nativeRunner
	}
	if runner != nil {
		workflowEngine, err := engine.New(store, artifacts, *workflowDir, runner)
		if err != nil {
			log.Fatal(err)
		}
		scheduler := engine.NewScheduler(store, workflowEngine, *concurrency, nil)
		var liveMu sync.Mutex
		lastConcurrency := *concurrency
		lastPolicy := engine.RunPolicy{MaxTurns: 300, MaxWall: 1800 * time.Second, AutoResumeWall: true, MaxResumes: 50}
		readInt := func(key string, fallback int) int {
			value, ok, err := configClient.IntValue(key)
			if err != nil {
				log.Printf("invalid live config %s: %v", key, err)
				return fallback
			}
			if !ok {
				return fallback
			}
			return value
		}
		scheduler.SetConcurrencySource(func() int {
			liveMu.Lock()
			defer liveMu.Unlock()
			lastConcurrency = readInt("autonomy.concurrency", lastConcurrency)
			return lastConcurrency
		})
		lastPerWorkflow := 1
		scheduler.SetPerWorkflowSource(func() int {
			liveMu.Lock()
			defer liveMu.Unlock()
			lastPerWorkflow = readInt("autonomy.per_workflow_concurrency", lastPerWorkflow)
			return lastPerWorkflow
		})
		scheduler.SetPolicySource(func() engine.RunPolicy {
			liveMu.Lock()
			defer liveMu.Unlock()
			lastPolicy.MaxTurns = readInt("autonomy.max_turns", lastPolicy.MaxTurns)
			lastPolicy.MaxWall = time.Duration(readInt("autonomy.max_wall_secs", int(lastPolicy.MaxWall/time.Second))) * time.Second
			lastPolicy.MaxResumes = readInt("autonomy.max_resumes", lastPolicy.MaxResumes)
			lastPolicy.StaleAbandon = time.Duration(readInt("autonomy.stale_abandon_secs", int(lastPolicy.StaleAbandon/time.Second))) * time.Second
			if value, ok, err := configClient.BoolValue("autonomy.auto_resume_cap_parks"); err != nil {
				log.Printf("invalid live config autonomy.auto_resume_cap_parks: %v", err)
			} else if ok {
				lastPolicy.AutoResumeWall = value
			}
			return lastPolicy
		})
		handler.SetSchedulerNotify(scheduler.Notify)
		handler.SetSchedulerCancel(scheduler.Cancel)
		if worktreeManager != nil {
			handler.SetWorktreeCleanup(worktreeManager.Cleanup)
		}
		go scheduler.Run(rootCtx)
		// Trigger definitions are live UI/config state. Re-read them every scan so
		// a saved workflow or run-policy change takes effect without a restart.
		go func() {
			for {
				handler.ScanTriggers(rootCtx)
				interval := configClient.Int("trigger.scan_interval_secs", 5)
				if interval < 1 {
					interval = 1
				}
				timer := time.NewTimer(time.Duration(interval) * time.Second)
				select {
				case <-rootCtx.Done():
					timer.Stop()
					return
				case <-timer.C:
				}
			}
		}()
	}

	if err := os.MkdirAll(filepath.Dir(*socket), 0o700); err != nil {
		log.Fatal(err)
	}
	_ = os.Remove(*socket)
	listener, err := net.Listen("unix", *socket)
	if err == nil {
		err = os.Chmod(*socket, 0o600)
	}
	if err != nil {
		log.Fatal(err)
	}
	defer listener.Close()
	metricsServer, err = observability.StartMetricsServer(metricsConfig, telemetry.MetricsHandler())
	if err != nil {
		log.Fatalf("initialize observability listener: %v", err)
	}
	if metricsServer != nil {
		log.Printf("Prometheus metrics listening on %s", metricsServer.Addr())
		go func() {
			if err := <-metricsServer.Done(); err != nil {
				log.Printf("observability listener stopped: %v", err)
			}
		}()
	}

	// Serve the workflow control stage over the event bus.
	//
	// This is the same mux the private AF_UNIX socket above serves; what changes
	// is that the C resource plane no longer needs a second transport to reach
	// it. The engine and its stores stay here -- only the way in moves -- so this
	// deletes src/server/wfe_http_proxy.c without relocating any state.
	//
	// The socket comes from the SAME value the store attach used, not from the
	// environment again. --module-bus-socket already defaults to
	// AIMEE_MODULE_BUS_SOCKET, so reading the variable here served the env case
	// and silently declined the flag case: an operator who passed the documented
	// flag got a process that attached its store, logged one line, and left every
	// /v1/workflow route and /v1/dev/submit answering 503. There is no missing
	// case left to tolerate -- the flag is checked above and the process does not
	// get this far without it.
	{
		busSocket := *moduleBusSocket
		go func() {
			err := bus.RunModuleProcess(rootCtx, bus.ModuleProcessConfig{
				SocketPath:     busSocket,
				ModuleName:     "workflows",
				PrincipalClass: 1,
				PrincipalRef:   20,
				Stages: []bus.ModuleStage{
					{EventKind: workflows.EventAdvance, StageID: workflows.StageAdvance},
					{EventKind: workflows.EventControl, StageID: workflows.StageControl},
					{EventKind: workflows.EventGateDecide, StageID: workflows.StageGateDecide},
					{EventKind: workflows.EventAutonomousRoute, StageID: workflows.StageAutonomousRoute},
				},
				Handler: workflows.NewHandler(handler),
			})
			if err != nil && rootCtx.Err() == nil {
				log.Printf("workflow control stage stopped: %v", err)
			}
		}()
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		rootCancel()
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		if metricsServer != nil {
			_ = metricsServer.Shutdown(ctx)
		}
		_ = server.Shutdown(ctx)
	}()
	log.Printf("Go aimee-server listening on %s", listener.Addr())
	if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
		log.Printf("serve: %v", err)
		return
	}
}

func envInt(name string, fallback int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed < 1 {
		return fallback
	}
	return parsed
}

// attachWithRetry dials the module bus until it answers or the deadline passes.
// The socket is created by the daemon and the engine may be started beside it,
// so "not there yet" is an ordinary boot state rather than a misconfiguration.
func attachWithRetry(ctx context.Context, socket string, within time.Duration) (*bus.Client, error) {
	deadline := time.Now().Add(within)
	var last error
	for {
		attached, err := bus.ConnectClient(ctx, socket, engine.BusPrincipalClass,
			engine.WFEBusPrincipalRef)
		if err == nil {
			return attached, nil
		}
		last = err
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("after %s: %w", within, last)
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
}

// waitForStore blocks until the DB1 module answers a trivial read. Logged once
// when it has to wait, because a slow start and a store that never arrives look
// identical from outside until something asks.
func waitForStore(ctx context.Context, store *db1.Store, within time.Duration) error {
	deadline := time.Now().Add(within)
	announced := false
	for {
		if _, err := store.ActiveRootCount(ctx); err == nil {
			if announced {
				log.Printf("DB1 module is serving; workflow engine starting")
			}
			return nil
		} else if !announced {
			log.Printf("waiting for the DB1 module to serve the store: %v", err)
			announced = true
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("no answer within %s", within)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
}

func waitForConfig(ctx context.Context, client *appconfig.Client, within time.Duration) error {
	deadline := time.Now().Add(within)
	announced := false
	for {
		if _, err := client.Values(); err == nil {
			if announced {
				log.Printf("config module is serving; workflow engine starting")
			}
			return nil
		} else if !announced {
			log.Printf("waiting for the config module to serve configuration: %v", err)
			announced = true
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("no answer within %s", within)
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(250 * time.Millisecond):
		}
	}
}
