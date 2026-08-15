package bus

import (
	"context"
	"errors"
	"fmt"
	"log"
	"strings"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"
)

const (
	moduleMaxInFlight  = 16
	moduleIdle         = time.Millisecond
	moduleConnectRetry = 100 * time.Millisecond
	// Well inside the host's 30s stale window, so a client stays admitted
	// across an idle stretch without heartbeating hot.
	clientHeartbeatInterval = 5 * time.Second
)

var (
	ErrModuleConfig  = errors.New("bus: invalid module process configuration")
	ErrModuleRuntime = errors.New("bus: module process runtime failed")
)

// ModuleStage binds one event kind to the stage identifier carried in its AMOD
// envelope. A process may serve several stages, but every kind is unambiguous.
type ModuleStage struct {
	EventKind uint32
	StageID   uint32
}

// ModuleInvocation is the transport-owned state visible to a module handler.
// A handler should call Cancelled around bounded units of work.
type ModuleInvocation struct {
	StageID    uint32
	DeadlineNS uint64
	TraceID    uint64
	cancelled  *cancelFlag
}

// Cancelled reports a bus cancellation or an expired absolute CLOCK_MONOTONIC
// deadline. Modules do not need to own either transport concern.
func (i ModuleInvocation) Cancelled() bool {
	if i.cancelled != nil && i.cancelled.set.Load() {
		return true
	}
	now := monotonicNowNS()
	return now != 0 && i.DeadlineNS != 0 && now >= i.DeadlineNS
}

// Remaining clamps work to both a stage-owned limit and the transport's
// absolute CLOCK_MONOTONIC deadline. A missing or unreadable transport clock
// leaves the stage-owned limit in force.
func (i ModuleInvocation) Remaining(limit time.Duration) time.Duration {
	if limit <= 0 {
		return 0
	}
	now := monotonicNowNS()
	if i.DeadlineNS == 0 || now == 0 {
		return limit
	}
	if now >= i.DeadlineNS {
		return 0
	}
	remaining := i.DeadlineNS - now
	if remaining < uint64(limit) {
		return time.Duration(remaining)
	}
	return limit
}

// ModuleHandler implements one or more stages. Non-OK results never carry a
// body. A response over ModuleMessageMaxBody is converted to Internal.
type ModuleHandler func(ModuleInvocation, []byte) ([]byte, ModuleStatus)

// ModuleProcessConfig is the language-neutral process identity and stage table
// materialized by the module inventory.
type ModuleProcessConfig struct {
	SocketPath     string
	ModuleName     string
	PrincipalClass uint32
	PrincipalRef   uint32
	Stages         []ModuleStage
	Handler        ModuleHandler
}

type cancelFlag struct{ set atomic.Bool }

type moduleAssembly struct {
	eventKind uint32
	message   ModuleMessage
	body      []byte
}

type moduleWork struct {
	eventKind     uint32
	correlationID uint64
	invocation    ModuleInvocation
	cancelled     *cancelFlag
}

type moduleResult struct {
	work   *moduleWork
	body   []byte
	status ModuleStatus
}

// moduleBus is deliberately the same narrow surface used by the runtime. The
// real implementation is Client; the interface keeps protocol tests independent
// of mmap setup without creating a second host implementation.
type moduleBus interface {
	Poll() (Event, bool, error)
	ReplyFragment(kind uint32, correlation uint64, payload []byte, more bool) error
	Heartbeat(now uint64)
	EpochChanged() bool
	moduleInlineBudget() uint32
}

func (c *Client) moduleInlineBudget() uint32 { return c.inlineBudget }

func monotonicNowNS() uint64 {
	var now unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &now); err != nil {
		return 0
	}
	return uint64(now.Sec)*1_000_000_000 + uint64(now.Nsec)
}

func validateModuleConfig(config ModuleProcessConfig) (map[uint32]uint32, error) {
	if config.SocketPath == "" || config.ModuleName == "" || config.PrincipalClass == 0 ||
		config.PrincipalRef == 0 || len(config.Stages) == 0 {
		return nil, ErrModuleConfig
	}
	stages := make(map[uint32]uint32, len(config.Stages))
	for _, stage := range config.Stages {
		if stage.EventKind == 0 || stage.StageID == 0 {
			return nil, ErrModuleConfig
		}
		if _, duplicate := stages[stage.EventKind]; duplicate {
			return nil, ErrModuleConfig
		}
		stages[stage.EventKind] = stage.StageID
	}
	return stages, nil
}

func connectModule(ctx context.Context, config ModuleProcessConfig) (*Client, error) {
	for {
		fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
		if err != nil {
			return nil, err
		}
		err = unix.Connect(fd, &unix.SockaddrUnix{Name: config.SocketPath})
		if err == nil {
			client, attachErr := AttachAs(fd, config.PrincipalClass, config.PrincipalRef)
			unix.Close(fd)
			return client, attachErr
		}
		unix.Close(fd)
		// A daemon restart can leave its old socket pathname in place until the
		// new host replaces it. The supervisor sees a socket and launches us in
		// that window; wait for the replacement listener instead of reporting a
		// spurious module crash. Missing sockets and policy errors still fail
		// immediately so configuration mistakes remain visible.
		if !errors.Is(err, unix.ECONNREFUSED) {
			return nil, err
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(moduleConnectRetry):
		}
	}
}

// RunModuleProcess attaches to a daemon's local bus and serves until ctx is
// cancelled or the host epoch changes. The attach socket is descriptor transport
// only; after AttachAs returns all traffic uses the shared mappings.
func RunModuleProcess(ctx context.Context, config ModuleProcessConfig) error {
	stages, err := validateModuleConfig(config)
	if err != nil {
		return err
	}
	if ctx == nil {
		return ErrModuleConfig
	}
	client, err := connectModule(ctx, config)
	if err != nil {
		if ctx.Err() != nil {
			return nil
		}
		return fmt.Errorf("%w: %s attach: %v", ErrModuleRuntime, config.ModuleName, err)
	}
	defer client.Detach()
	return runModuleClient(ctx, config, stages, client)
}

func validModuleStatus(status ModuleStatus) bool { return status <= ModuleStatusInternal }

// moduleDetail renders whatever a failing handler returned as a short, printable
// reason. Handlers are not required to supply one.
func moduleDetail(response []byte) string {
	if len(response) == 0 {
		return "no detail"
	}
	const max = 300
	if len(response) > max {
		return strings.ToValidUTF8(string(response[:max]), "") + "..."
	}
	return strings.ToValidUTF8(string(response), "")
}

func runHandler(done chan<- moduleResult, work *moduleWork, handler ModuleHandler, body []byte) {
	response, status := handler(work.invocation, body)
	if !validModuleStatus(status) || uint64(len(response)) > uint64(ModuleMessageMaxBody) {
		status = ModuleStatusInternal
		response = nil
	} else if status != ModuleStatusOK {
		// A non-OK reply carries no body, so whatever the handler said about the
		// failure stops here. Say it on the way past: a caller that receives a
		// bare status has nothing to report but the number, and a stage that
		// refuses every request looks identical to one that is merely slow.
		log.Printf("module stage %d (kind %d) failed with status %d: %s",
			work.invocation.StageID, work.eventKind, status, moduleDetail(response))
		response = nil
	}
	if work.cancelled.set.Load() {
		status = ModuleStatusCancelled
		response = nil
	} else if work.invocation.DeadlineNS != 0 {
		now := monotonicNowNS()
		if now != 0 && now >= work.invocation.DeadlineNS {
			status = ModuleStatusDeadlineExceeded
			response = nil
		}
	}
	done <- moduleResult{work: work, body: response, status: status}
}

func replyModuleResult(ctx context.Context, client moduleBus, kind uint32, correlation uint64,
	stageID uint32, traceID uint64, status ModuleStatus, body []byte) error {
	if !validModuleStatus(status) || uint64(len(body)) > uint64(ModuleMessageMaxBody) {
		status = ModuleStatusInternal
		body = nil
	} else if status != ModuleStatusOK {
		body = nil
	}
	budget := client.moduleInlineBudget()
	if budget <= ModuleMessageHeaderLen {
		return ErrModuleRuntime
	}
	chunkCapacity := int(budget) - ModuleMessageHeaderLen
	first := true
	for offset := 0; first || offset < len(body); {
		first = false
		part := len(body) - offset
		if part > chunkCapacity {
			part = chunkCapacity
		}
		more := offset+part < len(body)
		payload := make([]byte, ModuleMessageHeaderLen+part)
		message := ModuleMessage{Operation: ModuleOpResult, Status: status, StageID: stageID,
			BodyLen: uint32(part), TraceID: traceID}
		if _, err := message.Encode(payload); err != nil {
			return err
		}
		copy(payload[ModuleMessageHeaderLen:], body[offset:offset+part])
		for {
			err := client.ReplyFragment(kind, correlation, payload, more)
			if err == nil {
				break
			}
			if !errors.Is(err, ErrWouldBlock) {
				return err
			}
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(moduleIdle):
			}
			if client.EpochChanged() {
				return ErrEpoch
			}
			now := monotonicNowNS()
			if now != 0 {
				client.Heartbeat(now)
			}
		}
		offset += part
	}
	return nil
}

func invalidModuleRequest(ctx context.Context, client moduleBus, event Event, stageID uint32,
	traceID uint64) error {
	if stageID == 0 {
		stageID = 1
	}
	return replyModuleResult(ctx, client, event.Frame.EventKind, event.Frame.CorrelationID,
		stageID, traceID, ModuleStatusInvalidRequest, nil)
}

func finishModuleWork(ctx context.Context, client moduleBus, jobs map[uint64]*moduleWork,
	result moduleResult) error {
	delete(jobs, result.work.correlationID)
	return replyModuleResult(ctx, client, result.work.eventKind, result.work.correlationID,
		result.work.invocation.StageID, result.work.invocation.TraceID, result.status, result.body)
}

func runModuleClient(ctx context.Context, config ModuleProcessConfig, stages map[uint32]uint32,
	client moduleBus) error {
	if ctx == nil || client == nil {
		return ErrModuleConfig
	}
	assemblies := make(map[uint64]*moduleAssembly)
	jobs := make(map[uint64]*moduleWork)
	done := make(chan moduleResult, moduleMaxInFlight)

	for {
		for {
			select {
			case result := <-done:
				if err := finishModuleWork(ctx, client, jobs, result); err != nil {
					return err
				}
				continue
			default:
			}
			break
		}
		select {
		case <-ctx.Done():
			for _, work := range jobs {
				work.cancelled.set.Store(true)
			}
			for len(jobs) > 0 {
				result := <-done
				delete(jobs, result.work.correlationID)
			}
			return nil
		default:
		}
		if client.EpochChanged() {
			return nil
		}
		now := monotonicNowNS()
		if now != 0 {
			client.Heartbeat(now)
		}

		event, ok, err := client.Poll()
		if err != nil {
			if errors.Is(err, ErrEpoch) {
				return nil
			}
			return err
		}
		if !ok {
			time.Sleep(moduleIdle)
			continue
		}
		correlation := event.Frame.CorrelationID
		if event.Frame.HdrFlags&FCancel != 0 {
			if work := jobs[correlation]; work != nil {
				work.cancelled.set.Store(true)
			}
			delete(assemblies, correlation)
			continue
		}
		if event.Frame.HdrFlags&FRequest == 0 {
			continue
		}

		expectedStage := stages[event.Frame.EventKind]
		message, decodeErr := DecodeModuleMessage(event.Payload)
		assembly := assemblies[correlation]
		invalid := expectedStage == 0 || decodeErr != nil || message.Operation != ModuleOpInvoke ||
			message.StageID != expectedStage || jobs[correlation] != nil
		if !invalid && assembly != nil {
			invalid = assembly.eventKind != event.Frame.EventKind ||
				assembly.message.StageID != message.StageID ||
				assembly.message.DeadlineNS != message.DeadlineNS ||
				assembly.message.TraceID != message.TraceID
		}
		if invalid {
			delete(assemblies, correlation)
			if err := invalidModuleRequest(ctx, client, event, expectedStage, 0); err != nil {
				return err
			}
			continue
		}
		if assembly == nil {
			if len(assemblies) >= moduleMaxInFlight {
				if err := replyModuleResult(ctx, client, event.Frame.EventKind, correlation,
					expectedStage, message.TraceID, ModuleStatusInternal, nil); err != nil {
					return err
				}
				continue
			}
			assembly = &moduleAssembly{eventKind: event.Frame.EventKind, message: message}
			assemblies[correlation] = assembly
		}
		body := event.Payload[ModuleMessageHeaderLen : ModuleMessageHeaderLen+int(message.BodyLen)]
		if uint64(len(assembly.body))+uint64(len(body)) > uint64(ModuleMessageMaxBody) {
			delete(assemblies, correlation)
			if err := replyModuleResult(ctx, client, event.Frame.EventKind, correlation,
				expectedStage, message.TraceID, ModuleStatusInternal, nil); err != nil {
				return err
			}
			continue
		}
		assembly.body = append(assembly.body, body...)
		if event.Frame.HdrFlags&FMore != 0 {
			continue
		}
		delete(assemblies, correlation)
		if message.DeadlineExpired(now) {
			if err := replyModuleResult(ctx, client, event.Frame.EventKind, correlation,
				expectedStage, message.TraceID, ModuleStatusDeadlineExceeded, nil); err != nil {
				return err
			}
			continue
		}
		if config.Handler == nil {
			if err := replyModuleResult(ctx, client, event.Frame.EventKind, correlation,
				expectedStage, message.TraceID, ModuleStatusCapabilityAbsent, nil); err != nil {
				return err
			}
			continue
		}
		if len(jobs) >= moduleMaxInFlight {
			if err := replyModuleResult(ctx, client, event.Frame.EventKind, correlation,
				expectedStage, message.TraceID, ModuleStatusInternal, nil); err != nil {
				return err
			}
			continue
		}
		cancelled := &cancelFlag{}
		work := &moduleWork{eventKind: event.Frame.EventKind, correlationID: correlation,
			cancelled: cancelled, invocation: ModuleInvocation{StageID: expectedStage,
				DeadlineNS: message.DeadlineNS, TraceID: message.TraceID, cancelled: cancelled}}
		jobs[correlation] = work
		requestBody := append([]byte(nil), assembly.body...)
		go runHandler(done, work, config.Handler, requestBody)
	}
}
