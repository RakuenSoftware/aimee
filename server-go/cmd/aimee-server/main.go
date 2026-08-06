package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/internal/api"
	appconfig "github.com/JBailes/aimee/server-go/internal/config"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/engine"
	"github.com/JBailes/aimee/server-go/internal/wfe"
	roundtablemod "github.com/JBailes/aimee/server-go/modules/roundtable"
)

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
	dbPath := flag.String("db", "", "DB1 SQLite path")
	runnerURL := flag.String("runner-url", os.Getenv("AIMEE_WFE_RUNNER_URL"),
		"typed WFE runner endpoint; empty keeps execution disabled")
	runnerSocket := flag.String("runner-socket", os.Getenv("AIMEE_WFE_RUNNER_SOCKET"),
		"optional Unix socket for the typed WFE runner")
	agentURL := flag.String("agent-service-url", os.Getenv("AIMEE_AGENT_SERVICE_URL"),
		"agent resource-plane base URL used by the native Go WFE runner")
	agentSocket := flag.String("agent-service-socket", os.Getenv("AIMEE_AGENT_SERVICE_SOCKET"),
		"agent resource-plane Unix socket used by the native Go WFE runner")
	workflowDir := flag.String("workflow-dir", "", "workflow definition directory")
	moduleBusSocket := flag.String("module-bus-socket", os.Getenv("AIMEE_MODULE_BUS_SOCKET"),
		"daemon module bus socket; reviews are requested over it")
	configPath := flag.String("config", "", "aimee.yaml path")
	concurrency := flag.Int("workflow-concurrency", envInt("AIMEE_AUTONOMY_CONCURRENCY", 5),
		"maximum concurrent work items across the whole WFE (total agent budget)")
	flag.Parse()
	if *dbPath == "" {
		*dbPath = filepath.Join(*home, "aimee.db")
	}
	if *socket == "" {
		*socket = filepath.Join(*home, "aimee-server.sock")
	}
	if *workflowDir == "" {
		*workflowDir = filepath.Join(*home, "workflows")
	}
	if *configPath == "" {
		*configPath = filepath.Join(*home, "aimee.yaml")
	}
	if err := os.MkdirAll(*home, 0o700); err != nil {
		log.Fatalf("create aimee home: %v", err)
	}

	store, err := db1.Open(*dbPath)
	if err != nil {
		log.Fatal(err)
	}
	defer store.Close()
	artifacts, err := wfe.NewArtifactStore(filepath.Join(*home, "wfe-artifacts"))
	if err != nil {
		log.Fatal(err)
	}
	handler, err := api.New(store, artifacts, *workflowDir)
	if err != nil {
		log.Fatal(err)
	}
	configStore, err := appconfig.NewStore(*configPath)
	if err != nil {
		log.Fatal(err)
	}
	handler.SetConfigStore(configStore)
	// The WFE control plane is deliberately Unix-socket-only. Credentials must
	// never be carried in this long-lived process's argv or environment; the
	// socket's ownership and 0600 mode are the authentication boundary.
	server := &http.Server{Handler: handler, ReadHeaderTimeout: 15 * time.Second}
	rootCtx, rootCancel := context.WithCancel(context.Background())
	defer rootCancel()
	var runner engine.Runner
	var agentClient *engine.HTTPAgentClient
	var worktreeManager *engine.WorktreeManager
	if *runnerURL != "" {
		runner, err = engine.NewHTTPRunner(engine.HTTPRunnerConfig{
			Endpoint: *runnerURL, UnixSocket: *runnerSocket,
		})
		if err != nil {
			log.Fatal(err)
		}
	} else if *agentURL != "" || *agentSocket != "" {
		agents, clientErr := engine.NewHTTPAgentClient(engine.AgentHTTPConfig{
			BaseURL: *agentURL, UnixSocket: *agentSocket,
			PendingTimeoutSource: func() time.Duration {
				return time.Duration(configStore.Int("autonomy.delegate_pending_secs", 120)) * time.Second
			},
		})
		if clientErr != nil {
			log.Fatal(clientErr)
		}
		agentClient = agents
		worktrees, worktreeErr := engine.NewWorktreeManager(store, filepath.Join(*home, "wfe-worktrees"))
		if worktreeErr != nil {
			log.Fatal(worktreeErr)
		}
		worktreeManager = worktrees
		workflowRegistry, registryErr := wfe.NewRegistry(*workflowDir)
		if registryErr != nil {
			log.Fatal(registryErr)
		}
		forge, forgeErr := engine.NewHTTPForge(engine.HTTPForgeConfig{
			BaseURL: *agentURL, UnixSocket: *agentSocket,
		})
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
		if agentClient != nil {
			// The plane client holds no database handle, so the terminal sweep --
			// which reads the Go-owned lifecycle tables -- composes it with the store
			// here instead of the client carrying one.
			scheduler.SetTerminalCancellation(engine.NewTerminalJobCanceller(store, agentClient).CancelTerminalJobs)
		}
		var liveMu sync.Mutex
		lastConcurrency := *concurrency
		lastPolicy := engine.RunPolicy{MaxTurns: 300, MaxWall: 1800 * time.Second, AutoResumeWall: true, MaxResumes: 50}
		readInt := func(key string, fallback int) int {
			value, ok, err := configStore.IntValue(key)
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
			if value, ok, err := configStore.BoolValue("autonomy.auto_resume_cap_parks"); err != nil {
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
				interval := configStore.Int("trigger.scan_interval_secs", 5)
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

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-stop
		rootCancel()
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		_ = server.Shutdown(ctx)
	}()
	log.Printf("Go aimee-server listening on %s", listener.Addr())
	if err := server.Serve(listener); err != nil && err != http.ErrServerClosed {
		log.Fatal(fmt.Errorf("serve: %w", err))
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
