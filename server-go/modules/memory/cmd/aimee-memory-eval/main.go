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

func run(ctx context.Context, schemaPath string, dimension int, input io.Reader, output io.Writer) (err error) {
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
	data, err := memory.NewPostgresDataStore(db, memory.PlacementKB)
	if err != nil {
		return err
	}
	// No network or model executor is installed by this local transport. Owner
	// operations requiring one must report its absence rather than fake vectors.
	return serve(ctx, input, output, memory.NewHandler(nil, memory.WithDataStore(memory.PlacementKB, data)))
}

func main() {
	schema := flag.String("schema", "", "path to the packaged KB schema.sql")
	dimension := flag.Int("embedding-dim", 0, "configured vector width (1..2000)")
	flag.Parse()
	if flag.NArg() != 0 {
		fmt.Fprintln(os.Stderr, "evaluation accepts JSON lines on stdin, not positional arguments")
		os.Exit(1)
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	stop := context.AfterFunc(ctx, func() { _ = os.Stdin.Close() })
	defer stop()
	if err := run(ctx, *schema, *dimension, os.Stdin, os.Stdout); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
