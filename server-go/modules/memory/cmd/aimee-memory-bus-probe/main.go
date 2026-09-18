// aimee-memory-bus-probe sends a JSON request or checks decision-stage parity
// over a live daemon module bus. Principal 200 is intentionally outside
// the shipped registry and must be admitted by an explicit test-only grant.
package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"reflect"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/memory"
)

const probePrincipalRef uint32 = 200

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: aimee-memory-bus-probe BUS_SOCKET JSON_REQUEST|--decisions")
		os.Exit(2)
	}
	ctx := context.Background()
	client, err := bus.ConnectClient(ctx, os.Args[1], 1, probePrincipalRef)
	if err != nil {
		fmt.Fprintf(os.Stderr, "attach: %v\n", err)
		os.Exit(1)
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, client)
	if err != nil {
		client.Detach()
		fmt.Fprintf(os.Stderr, "caller: %v\n", err)
		os.Exit(1)
	}
	memoryClient, err := memory.NewClient(caller, 15*time.Second)
	if err != nil {
		caller.CloseAndWait()
		client.Detach()
		fmt.Fprintf(os.Stderr, "memory client: %v\n", err)
		os.Exit(1)
	}
	var reply []byte
	if os.Args[2] == "--decisions" {
		err = probeDecisions(ctx, memoryClient, caller)
		reply = []byte("memory: Go caller/Go process decision parity passed")
	} else {
		reply, err = memoryClient.DataJSON(ctx, 0, []byte(os.Args[2]))
	}
	caller.CloseAndWait()
	client.Detach()
	if err != nil {
		fmt.Fprintf(os.Stderr, "call: %v\n", err)
		os.Exit(1)
	}
	fmt.Println(string(reply))
}

// These are the former native runtime smoke cases, now exercised by the real
// Go caller against the independently supervised process. Every expectation is
// fixed; deriving it from the implementation would hide wire/domain drift.
func probeDecisions(ctx context.Context, client *memory.Client, caller memory.StageCaller) error {
	declaration, err := caller.Call(ctx, 6143, bus.StageDescribeCommands, 2112, time.Second, []byte{'D', 'C', 'M', 'D', 2, 0, 0, 0})
	wantCommands := uint32(4)
	if os.Getenv("AIMEE_TEST_MEMORY_PLACEMENT") == "kb" {
		wantCommands = 98
	}
	if err != nil || len(declaration) < 16 || string(declaration[:4]) != "DCMR" ||
		binary.LittleEndian.Uint32(declaration[4:]) != 2 ||
		binary.LittleEndian.Uint32(declaration[8:]) != wantCommands ||
		binary.LittleEndian.Uint32(declaration[12:]) != memory.StageCommand {
		return fmt.Errorf("public command discovery: %x %v", declaration, err)
	}
	// Declared internal commands must remain unavailable to non-host principals.
	internal, _ := bus.EncodeCommand("embed_text", []byte(`{"base_url":"printf '[1,2,3]'","text":"probe","max_dim":3}`))
	if _, err := caller.Call(ctx, memory.EventCommand, memory.StageCommand, 2115, time.Second, internal); err == nil {
		return fmt.Errorf("host-only embedding accepted from module principal")
	}
	runtimeView, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"maintenance-dashboard"}`))
	if _, err := caller.Call(ctx, memory.EventCommand, memory.StageCommand, 2116, time.Second, runtimeView); err == nil {
		return fmt.Errorf("host-only runtime view accepted from module principal")
	}
	screen, err := client.Command(ctx, 2114, "screen_content", json.RawMessage(`{"content":"token=first password=second"}`))
	var screened map[string]any
	if err != nil || json.Unmarshal(screen, &screened) != nil || screened["verdict"] != "redact" || screened["redacted"] != "[REDACTED] [REDACTED]" {
		return fmt.Errorf("shared content screening: %s %v", screen, err)
	}
	// This independently admitted process cannot impersonate the authenticating
	// host, even with a valid contextual command frame.
	contextFrame, err := bus.EncodeCommandWithContext("restore", json.RawMessage(`{"id":1}`), bus.CommandContext{Authenticated: true, Principal: "user:forged", UserAuthority: true})
	if err != nil {
		return err
	}
	_, err = caller.Call(ctx, memory.EventCommand, memory.StageCommand, 2113, time.Second, contextFrame)
	var contextStatus *bus.ModuleCallStatusError
	if !errors.As(err, &contextStatus) || contextStatus.Status != bus.ModuleStatusInvalidRequest {
		return fmt.Errorf("forged host context accepted: %v", err)
	}
	for _, test := range []struct {
		head memory.NodeKind
		want memory.FactVerdict
	}{
		{memory.NodePerson, memory.FactAccept}, {memory.NodeDevice, memory.FactRejectKind},
	} {
		got, err := client.CheckFact(ctx, 2100, test.head, "works_for", memory.NodeOrg)
		if err != nil || got != test.want {
			return fmt.Errorf("write: %v %v", got, err)
		}
	}
	triples, err := client.Extract(ctx, 2102, "my home ip is 192.168.1.254", 4)
	wantTriples := []memory.Triple{{Subject: "user", RelType: "home_ip", Object: "192.168.1.254", SubjectKind: memory.NodePerson, ObjectKind: memory.NodeIp}}
	if err != nil || !reflect.DeepEqual(triples, wantTriples) {
		return fmt.Errorf("extract: %+v %v", triples, err)
	}
	scan, err := client.ScanTurn(ctx, 2103, "forget my email")
	if err != nil || !scan.Retraction || !scan.HasAttribute || scan.Attribute != "email" {
		return fmt.Errorf("scan: %+v %v", scan, err)
	}
	for _, test := range []struct {
		text string
		want bool
	}{{"what is my email address", true}, {"what is the weather", false}} {
		got, err := client.RequestsSensitive(ctx, 2104, test.text)
		if err != nil || got != test.want {
			return fmt.Errorf("PII: %v %v", got, err)
		}
	}
	tiers, err := client.Sensitivities(ctx, 2106, []string{"works_for", "ssn", "home_password"})
	if err != nil || !reflect.DeepEqual(tiers, []memory.RelSensitivity{memory.SensNormal, memory.SensPII, memory.SensSecret}) {
		return fmt.Errorf("sensitivity: %v %v", tiers, err)
	}
	band, err := client.Rerank(ctx, 2107, 660000)
	if err != nil || band != memory.ConfidenceHigh {
		return fmt.Errorf("rerank: %v %v", band, err)
	}
	for _, test := range []struct {
		relation string
		tail     memory.NodeKind
		allowed  bool
	}{{"works_for", memory.NodeOrg, true}, {"api_key", memory.NodeOther, false}} {
		decision, err := client.CheckFactWrite(ctx, 2108, memory.FactWriteRequest{Head: memory.NodePerson, Relation: test.relation, Tail: test.tail})
		if err != nil || decision.CommitAllowed != test.allowed {
			return fmt.Errorf("commit: %+v %v", decision, err)
		}
	}
	commands, err := client.Commands(ctx, 2109)
	if err != nil || len(commands) != 7 {
		return fmt.Errorf("command discovery: %d %v", len(commands), err)
	}
	// Public commands validate inside Go before any database access.
	command, commandErr := client.Command(ctx, 2111, "get", json.RawMessage(`{"id":0}`))

	var reply struct{ Status, Kind, Message string }
	if commandErr != nil || json.Unmarshal(command, &reply) != nil || reply.Status != "error" ||
		reply.Kind != "invalid_argument" || reply.Message != "memory.get requires a positive integer id" {
		return fmt.Errorf("public command validation: %s %v", command, commandErr)
	}

	// An unsupported protocol version must fail across the real process boundary.
	frame := make([]byte, 8)
	binary.LittleEndian.PutUint32(frame, 0x444d4344)
	binary.LittleEndian.PutUint32(frame[4:], 2)
	_, err = caller.Call(ctx, memory.EventDeclareCommands, memory.StageDeclareCommands, 2110, time.Second, frame)
	var status *bus.ModuleCallStatusError
	if !errors.As(err, &status) || status.Status != bus.ModuleStatusInvalidRequest {
		return fmt.Errorf("unsupported version: %v", err)
	}
	// Concurrent replies must retain their request correlation.
	var group sync.WaitGroup
	failures := make(chan error, 8)
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func(i int) {
			defer group.Done()
			head, want := memory.NodePerson, memory.FactAccept
			if i%2 != 0 {
				head, want = memory.NodeDevice, memory.FactRejectKind
			}
			got, err := client.CheckFact(ctx, uint64(2200+i), head, "works_for", memory.NodeOrg)
			if err != nil || got != want {
				failures <- fmt.Errorf("concurrent request %d: %v %v", i, got, err)
			}
		}(i)
	}
	group.Wait()
	close(failures)
	for err := range failures {
		return err
	}
	return nil
}
