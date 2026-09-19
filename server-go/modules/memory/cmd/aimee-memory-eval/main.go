// aimee-memory-eval runs the shared Go KB memory owner in an isolated database.
// Its JSON-lines protocol is a standalone evaluation transport, not a public API.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
	"github.com/JBailes/aimee/server-go/modules/postgres"
)

const maxEvaluationLine = 1 << 20

type evaluationRequest struct {
	Stage   string          `json:"stage"`
	Command string          `json:"command,omitempty"`
	Body    json.RawMessage `json:"body"`
}

type evaluationReply struct {
	Status bus.ModuleStatus `json:"status"`
	Body   json.RawMessage  `json:"body"`
}

func serve(ctx context.Context, input io.Reader, output io.Writer, handler bus.ModuleHandler) error {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), maxEvaluationLine)
	encoder := json.NewEncoder(output)
	for scanner.Scan() {
		if err := ctx.Err(); err != nil {
			return err
		}
		var request evaluationRequest
		decoder := json.NewDecoder(bytes.NewReader(scanner.Bytes()))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&request); err != nil {
			return fmt.Errorf("evaluation request: %w", err)
		}
		var extra any
		if err := decoder.Decode(&extra); err != io.EOF {
			return errors.New("evaluation request: expected one JSON object per line")
		}
		if len(request.Body) == 0 || request.Body[0] != '{' {
			return errors.New("evaluation request: body must be an object")
		}
		body := []byte(request.Body)
		var stage uint32
		switch request.Stage {
		case "data":
			stage = memory.StageData
		case "command":
			stage = memory.StageCommand
			var err error
			body, err = bus.EncodeCommand(request.Command, body)
			if err != nil {
				return err
			}
		default:
			return errors.New("evaluation request: stage must be data or command")
		}
		if request.Stage != "command" && request.Command != "" {
			return errors.New("evaluation request: command requires command stage")
		}
		body, status := handler(bus.ModuleInvocation{StageID: stage}, body)
		if stage == memory.StageCommand && len(body) > 0 {
			var err error
			body, err = bus.DecodeCommandResult(body)
			if err != nil {
				return err
			}
		}
		if len(body) == 0 {
			body = []byte("null")
		}
		if err := encoder.Encode(evaluationReply{Status: status, Body: body}); err != nil {
			return err
		}
	}
	return errors.Join(scanner.Err(), ctx.Err())
}

func run(ctx context.Context, schemaPath string, dimension int, input io.Reader, output io.Writer) error {
	return evaluationSession(ctx, schemaPath, dimension, func(db *postgres.EvaluationStore) error {
		module, err := memory.NewEvaluationModule(ctx, db, nil)
		if err != nil {
			return err
		}
		return serve(ctx, input, output, module.Handler)
	})
}

func evaluationSession(ctx context.Context, schemaPath string, dimension int, action func(*postgres.EvaluationStore) error) (err error) {
	if schemaPath == "" || dimension < 1 || dimension > 2000 {
		return errors.New("evaluation requires -schema (packaged KB schema) and -embedding-dim in 1..2000")
	}
	file, err := os.Open(schemaPath)
	if err != nil {
		return err
	}
	schema, err := io.ReadAll(io.LimitReader(file, 4<<20+1))
	closeErr := file.Close()
	if err = errors.Join(err, closeErr); err != nil {
		return err
	}
	if len(schema) > 4<<20 || !bytes.Contains(schema, []byte("__EMBED_DIM__")) {
		return errors.New("evaluation schema must be a bounded packaged KB schema with __EMBED_DIM__")
	}
	db, err := postgres.OpenEvaluationStore(ctx, strings.ReplaceAll(string(schema), "__EMBED_DIM__", strconv.Itoa(dimension)))
	if err != nil {
		return err
	}
	defer func() { err = errors.Join(err, db.Close()) }()
	return action(db)
}

func defaultSchema() string {
	if path := os.Getenv("AIMEE_EVAL_SCHEMA"); path != "" {
		return path
	}
	if executable, err := os.Executable(); err == nil {
		path := filepath.Join(filepath.Dir(executable), "..", "share", "aimee", "memory-eval-schema.sql")
		if info, err := os.Stat(path); err == nil && !info.IsDir() {
			return path
		}
	}
	return "src/modules/db2/c/schema.sql"
}

func main() {
	schema := flag.String("schema", defaultSchema(), "path to the packaged KB schema.sql")
	dimension := flag.Int("embedding-dim", 0, "configured vector width (1..2000)")
	suite := flag.String("suite", "", "isolated retrieval dataset: locomo or longmemeval")
	dataset := flag.String("dataset", "", "dataset JSON file")
	maxCases := flag.Int("max-cases", 0, "maximum conversations (LoCoMo) or questions (LongMemEval); zero means all")
	corpus := flag.String("corpus", "", "labelled corpus file; otherwise serve JSON lines")
	command := flag.String("embedding-command", "", "corpus embedder URL or configured command")
	socket := flag.String("module-bus-socket", os.Getenv("AIMEE_MODULE_BUS_SOCKET"), "governed egress bus socket for HTTP embedding")
	baseline := flag.String("baseline", "", "optional baseline file to compare or update")
	update := flag.Bool("update-baseline", false, "atomically replace the baseline after a successful evaluation")
	format := flag.String("format", "json", "corpus output: json or text")
	fields := flag.String("fields", "", "comma-separated output fields")
	profile := flag.String("profile", "", "response profile")
	agentExecutable := flag.String("agent-executable", "", "aimee CLI providing configured tool-free completions for QA")
	topK := flag.Int("top-k", 10, "QA context results (1..32)")
	tokenBudget := flag.Int("token-budget", 2000, "QA context token budget")
	reportFailures := flag.Bool("report-failures", false, "include QA failure details from the scored attempt")
	maxFailures := flag.Int("max-failures", 5, "maximum QA failure details (1..100)")
	missLimit := flag.Int("limit", 5, "miss report retrieval cutoff (1..20)")
	maxMisses := flag.Int("max-misses", 20, "maximum miss details (1..100)")
	flag.Parse()
	if flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "evaluation accepts JSON lines on stdin, not positional arguments")
		os.Exit(1)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	stop := context.AfterFunc(ctx, func() { _ = os.Stdin.Close() })
	defer stop()
	var err error
	if *suite != "" || *dataset != "" {
		if *corpus != "" || *baseline != "" || *update {
			err = errors.New("dataset evaluation cannot use corpus/baseline options")
		} else if strings.HasSuffix(*suite, "-misses") {
			err = runDatasetMisses(ctx, *schema, *dimension, *dataset, *suite, *maxCases, *command, *socket, *format, *fields, *profile, *missLimit, *maxMisses, os.Stdout)
		} else if strings.HasSuffix(*suite, "-qa") {
			err = runDatasetQA(ctx, *schema, *dimension, *dataset, *suite, *maxCases, *command, *socket, *format, *fields, *profile, qaOptions{*topK, *tokenBudget, *maxFailures, *reportFailures, agentModel(*agentExecutable)}, os.Stdout)
		} else {
			err = runDataset(ctx, *schema, *dimension, *dataset, *suite, *maxCases, *command, *socket, *format, *fields, *profile, os.Stdout)
		}
	} else if *maxCases != 0 {
		err = errors.New("max-cases requires a dataset suite")
	} else if *corpus == "" {
		if *command != "" || *baseline != "" || *update || *format != "json" {
			err = errors.New("corpus options require -corpus")
		} else {
			err = run(ctx, *schema, *dimension, os.Stdin, os.Stdout)
		}
	} else {
		err = runCorpus(ctx, *schema, *dimension, *corpus, *command, *socket, *baseline, *update, *format, *fields, *profile, os.Stdout)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
