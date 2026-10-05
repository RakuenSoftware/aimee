package egress

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"os"
	"os/exec"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// PrepareHandler authenticates a private Vault pipe before the owner becomes
// non-dumpable. The helper sends only a readiness marker at startup; credential
// requests begin after authenticated bus attach and process hardening.
// A failed pipe is never reopened by a hardened owner. Done asks its supervisor
// to restart the owner and establish a new authenticated process boundary.
func PrepareHandler(ctx context.Context) (bus.ModuleHandler, func(), <-chan struct{}, error) {
	helper := os.Getenv("AIMEE_EGRESS_CREDENTIAL_HELPER")
	if helper == "" {
		// Native launches without a Vault helper retain credential-free egress.
		return NewHandler(), func() {}, nil, nil
	}
	if helper != "/usr/local/bin/aimee-server" && helper != "/usr/local/bin/aimee-kb" {
		return nil, nil, nil, errors.New("credential helper is unavailable")
	}
	pipe, err := startVaultPipe(ctx, helper, os.Getenv("AIMEE_HOME"))
	if err != nil {
		return nil, nil, nil, err
	}
	resolver := &vaultCredentialResolver{run: pipe.request}
	return newHandlerWithCredentials(net.DefaultResolver, resolver), pipe.close, pipe.done, nil
}

type vaultPipe struct {
	command       *exec.Cmd
	input, output *os.File
	lock          chan struct{}
	done          chan struct{}
	once          sync.Once
}

func startVaultPipe(ctx context.Context, helper, home string) (*vaultPipe, error) {
	command := exec.Command(helper, "--egress-vault-resource")
	command.Env = []string{"AIMEE_HOME=" + home, "PATH=/usr/local/bin:/usr/bin:/bin"}
	inputRead, inputWrite, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	outputRead, outputWrite, err := os.Pipe()
	if err != nil {
		inputRead.Close()
		inputWrite.Close()
		return nil, err
	}
	command.Stdin, command.Stdout = inputRead, outputWrite
	pipe := &vaultPipe{command: command, input: inputWrite, output: outputRead,
		lock: make(chan struct{}, 1), done: make(chan struct{})}
	err = command.Start()
	inputRead.Close()
	outputWrite.Close()
	if err != nil {
		inputWrite.Close()
		outputRead.Close()
		return nil, errors.New("credential helper failed")
	}
	go func() { _ = command.Wait(); pipe.abort(); close(pipe.done) }()
	startup, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	stop := context.AfterFunc(startup, pipe.abort)
	defer stop()
	_ = pipe.output.SetReadDeadline(time.Now().Add(10 * time.Second))
	var ready [4]byte
	if _, err := io.ReadFull(pipe.output, ready[:]); err != nil || string(ready[:]) != "EVR1" {
		pipe.close()
		return nil, errors.New("credential helper attestation failed")
	}
	if startup.Err() != nil {
		pipe.close()
		return nil, startup.Err()
	}
	return pipe, nil
}

func (p *vaultPipe) abort() {
	p.once.Do(func() {
		p.input.Close()
		p.output.Close()
		_ = p.command.Process.Kill()
	})
}

func (p *vaultPipe) close() { p.abort(); <-p.done }

func (p *vaultPipe) request(ctx context.Context, name string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	select {
	case p.lock <- struct{}{}:
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-p.done:
		return nil, errors.New("credential pipe closed")
	}
	defer func() { <-p.lock }()
	if ctx.Err() != nil {
		return nil, ctx.Err()
	}
	stop := context.AfterFunc(ctx, p.abort)
	defer stop()
	deadline, _ := ctx.Deadline()
	_ = p.input.SetWriteDeadline(deadline)
	_ = p.output.SetReadDeadline(deadline)
	var size [4]byte
	binary.LittleEndian.PutUint32(size[:], uint32(len(name)))
	if _, err := p.input.Write(append(size[:], []byte(name)...)); err != nil {
		p.abort()
		return nil, errors.New("credential pipe failed")
	}
	if _, err := io.ReadFull(p.output, size[:]); err != nil {
		p.abort()
		return nil, errors.New("credential pipe failed")
	}
	length := binary.LittleEndian.Uint32(size[:])
	if length > maxResolvedCredentialBytes {
		p.abort()
		return nil, errors.New("invalid credential response")
	}
	value := make([]byte, length)
	if _, err := io.ReadFull(p.output, value); err != nil || ctx.Err() != nil {
		clear(value)
		p.abort()
		return nil, errors.New("credential pipe failed")
	}
	return value, nil
}
