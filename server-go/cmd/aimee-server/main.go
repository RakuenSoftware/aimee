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
	"syscall"
	"time"

	"github.com/JBailes/aimee/server-go/internal/api"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/internal/engine"
	"github.com/JBailes/aimee/server-go/internal/wfe"
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
	listen := flag.String("listen", "", "optional TCP listen address")
	dbPath := flag.String("db", "", "DB1 SQLite path")
	runnerURL := flag.String("runner-url", os.Getenv("AIMEE_WFE_RUNNER_URL"),
		"typed WFE runner endpoint; empty keeps execution disabled")
	runnerSocket := flag.String("runner-socket", os.Getenv("AIMEE_WFE_RUNNER_SOCKET"),
		"optional Unix socket for the typed WFE runner")
	workflowDir := flag.String("workflow-dir", "", "workflow definition directory")
	concurrency := flag.Int("workflow-concurrency", envInt("AIMEE_AUTONOMY_CONCURRENCY", 2),
		"maximum concurrent workflows")
	bearerToken := flag.String("bearer-token", os.Getenv("AIMEE_API_BEARER_TOKEN"),
		"bearer required by TCP and optional on Unix sockets")
	flag.Parse()
	if *dbPath == "" {
		*dbPath = filepath.Join(*home, "aimee.db")
	}
	if *socket == "" && *listen == "" {
		*socket = filepath.Join(*home, "aimee-server.sock")
	}
	if *workflowDir == "" {
		*workflowDir = filepath.Join(*home, "workflows")
	}
	if *listen != "" && *bearerToken == "" {
		log.Fatal("TCP listening requires --bearer-token or AIMEE_API_BEARER_TOKEN")
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
	handler := api.New(store, artifacts, *workflowDir)
	server := &http.Server{Handler: api.RequireBearer(handler, *bearerToken), ReadHeaderTimeout: 15 * time.Second}
	rootCtx, rootCancel := context.WithCancel(context.Background())
	defer rootCancel()
	if *runnerURL != "" {
		runner, err := engine.NewHTTPRunner(engine.HTTPRunnerConfig{
			Endpoint: *runnerURL, UnixSocket: *runnerSocket,
		})
		if err != nil {
			log.Fatal(err)
		}
		workflowEngine, err := engine.New(store, artifacts, *workflowDir, runner)
		if err != nil {
			log.Fatal(err)
		}
		scheduler := engine.NewScheduler(store, workflowEngine, *concurrency, nil)
		handler.SetSchedulerNotify(scheduler.Notify)
		go scheduler.Run(rootCtx)
	}

	var listener net.Listener
	if *socket != "" {
		if err := os.MkdirAll(filepath.Dir(*socket), 0o700); err != nil {
			log.Fatal(err)
		}
		_ = os.Remove(*socket)
		listener, err = net.Listen("unix", *socket)
		if err == nil {
			err = os.Chmod(*socket, 0o600)
		}
	} else {
		listener, err = net.Listen("tcp", *listen)
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
