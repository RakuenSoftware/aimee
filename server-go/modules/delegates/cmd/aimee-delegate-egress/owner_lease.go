package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

const ownerLeaseSchema = "aimee.container-lease.v1"
const ownerLeaseMax = (1 << 20) + 4096

type ownerLeaseRecord struct {
	Schema     string                         `json:"schema"`
	Kind       string                         `json:"kind"`
	Engine     string                         `json:"engine_endpoint,omitempty"`
	HelperSHA  string                         `json:"helper_sha256,omitempty"`
	Startup    string                         `json:"trusted_startup,omitempty"`
	Request    *delegates.DockerCreateRequest `json:"request,omitempty"`
	LeaseDev   string                         `json:"lease_dev,omitempty"`
	LeaseInode string                         `json:"lease_inode,omitempty"`
	ID         string                         `json:"container_id,omitempty"`
}
type ownerLease struct {
	file              *os.File
	engine, helperSHA string
	request           *delegates.DockerCreateRequest
	id                string
	validEnd          int64
	recovering        bool
	removed           bool
}

func openOwnerLease(fd int, check, socket string) (*ownerLease, error) {
	return openOwnerLeaseMode(fd, check, socket, false)
}

// Cleanup authenticates the retained descriptor against the durable intent's
// device/inode, not a pathname that may have disappeared after acquisition.
func openOwnerLeaseMode(fd int, check, socket string, retained bool) (*ownerLease, error) {
	if fd < 3 || !filepath.IsAbs(check) || filepath.Clean(check) != check || filepath.Base(check) != "lease.jsonl" || filepath.Dir(check) != filepath.Dir(socket) {
		return nil, errors.New("private creation lease descriptor required")
	}
	parent := filepath.Dir(check)
	owners := filepath.Dir(parent)
	if filepath.Base(owners) != "acp-owners" || len(filepath.Base(parent)) != 64 || strings.Trim(filepath.Base(parent), "0123456789abcdef") != "" {
		return nil, errors.New("private lease scope required")
	}
	if !retained {
		for _, directory := range []string{parent, owners} {
			info, err := os.Lstat(directory)
			if err != nil {
				return nil, err
			}
			stat, ok := info.Sys().(*syscall.Stat_t)
			if !ok || !info.IsDir() || info.Mode().Perm() != 0700 || info.Mode()&os.ModeSymlink != 0 || int(stat.Uid) != os.Geteuid() {
				return nil, errors.New("private lease scope posture differs")
			}
		}
	}
	syscall.CloseOnExec(fd)
	flags, _, failure := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_GETFD, 0)
	if failure != 0 || flags&syscall.FD_CLOEXEC == 0 {
		syscall.Close(fd)
		return nil, errors.New("private lease cannot be isolated from child execution")
	}
	file := os.NewFile(uintptr(fd), "private-creation-lease")
	if file == nil {
		return nil, errors.New("creation lease unavailable")
	}
	info, err := file.Stat()
	if err == nil {
		stat, ok := info.Sys().(*syscall.Stat_t)
		if !ok || !info.Mode().IsRegular() || info.Mode().Perm() != 0600 || int(stat.Uid) != os.Geteuid() || stat.Nlink > 1 || (!retained && stat.Nlink != 1) {
			err = errors.New("creation lease descriptor identity differs")
		}
		if err == nil && !retained {
			current, failure := os.Lstat(check)
			if failure != nil || !os.SameFile(info, current) || current.Mode()&os.ModeSymlink != 0 {
				err = errors.New("creation lease pathname differs")
			}
		}
	}

	if err != nil {
		file.Close()
		return nil, err
	}
	return &ownerLease{file: file}, nil
}
func (lease *ownerLease) append(record ownerLeaseRecord) error {
	record.Schema = ownerLeaseSchema
	encoded, err := json.Marshal(record)
	if err != nil || len(encoded) > 1<<20 {
		return errors.New("creation lease record exceeds bound")
	}
	if lease.validEnd > 0 {
		if err := lease.file.Truncate(lease.validEnd); err != nil {
			return err
		}
	}
	if _, err = lease.file.Seek(0, io.SeekEnd); err != nil {
		return err
	}
	encoded = append(encoded, '\n')
	if count, err := lease.file.Write(encoded); err != nil || count != len(encoded) {
		return errors.New("creation lease append incomplete")
	}
	if err := lease.file.Sync(); err != nil {
		return err
	}
	lease.validEnd, err = lease.file.Seek(0, io.SeekCurrent)
	return err
}
func (lease *ownerLease) Intent(request delegates.DockerCreateRequest) error {
	info, err := lease.file.Stat()
	if err != nil || info.Size() != 0 {
		return errors.New("fresh creation lease must be empty")
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return errors.New("creation lease source identity unavailable")
	}
	if err := lease.append(ownerLeaseRecord{Kind: "intent", Engine: lease.engine, HelperSHA: lease.helperSHA, Startup: "owner-io-idle-guard-v1", Request: &request, LeaseDev: strconv.FormatUint(uint64(stat.Dev), 10), LeaseInode: strconv.FormatUint(uint64(stat.Ino), 10)}); err != nil {
		return err
	}
	lease.request = &request
	return nil
}
func (lease *ownerLease) Created(id string) error {
	if lease.request == nil || lease.id != "" || len(id) != 64 || strings.Trim(id, "0123456789abcdef") != "" {
		return errors.New("invalid acknowledged creation identity")
	}
	kind := "created"
	if lease.recovering {
		kind = "resolved"
	}
	if err := lease.append(ownerLeaseRecord{Kind: kind, ID: id}); err != nil {
		return err
	}
	lease.id = id
	return nil
}
func (lease *ownerLease) read() error {
	lease.request = nil
	lease.id = ""
	lease.removed = false
	if _, err := lease.file.Seek(0, io.SeekStart); err != nil {
		return err
	}
	encoded, err := io.ReadAll(io.LimitReader(lease.file, ownerLeaseMax+1))
	if err != nil || len(encoded) > ownerLeaseMax {
		return errors.New("creation lease exceeds bound")
	}
	end := bytes.LastIndexByte(encoded, '\n')
	if end < 0 {
		return errors.New("durable creation intent unavailable")
	}
	lease.validEnd = int64(end + 1)
	records := bytes.Split(encoded[:end], []byte{'\n'})
	for index, line := range records {
		var record ownerLeaseRecord
		decoder := json.NewDecoder(bytes.NewReader(line))
		decoder.DisallowUnknownFields()
		if decoder.Decode(&record) != nil || decoder.Decode(new(any)) != io.EOF || record.Schema != ownerLeaseSchema {
			return errors.New("invalid complete creation lease record")
		}
		if index == 0 {
			if record.Kind != "intent" || record.Request == nil || record.Engine == "" || record.Startup != "owner-io-idle-guard-v1" || len(record.HelperSHA) != 64 || strings.Trim(record.HelperSHA, "0123456789abcdef") != "" || record.ID != "" {
				return errors.New("typed creation intent unavailable")
			}
			info, err := lease.file.Stat()
			if err != nil {
				return err
			}
			stat, ok := info.Sys().(*syscall.Stat_t)
			if !ok || record.LeaseDev != strconv.FormatUint(uint64(stat.Dev), 10) || record.LeaseInode != strconv.FormatUint(uint64(stat.Ino), 10) {
				return errors.New("retained lease source inode differs")
			}
			lease.engine, lease.helperSHA, lease.request = record.Engine, record.HelperSHA, record.Request
		} else {
			if record.Request != nil || record.Engine != "" || record.HelperSHA != "" || record.Startup != "" || record.LeaseDev != "" || record.LeaseInode != "" || len(record.ID) != 64 || strings.Trim(record.ID, "0123456789abcdef") != "" {
				return errors.New("invalid creation identity record")
			}
			if index == 1 && (record.Kind == "created" || record.Kind == "resolved") {
				lease.id = record.ID
			} else if index != 2 || record.Kind != "removed" || lease.id == "" || record.ID != lease.id {
				return errors.New("creation identity sequence differs")
			} else {
				lease.removed = true
			}
		}
	}
	return nil
}

func cleanupOwnerLease(ctx context.Context, args []string, run delegates.CommandRunner) error {
	flags := flag.NewFlagSet("cleanup-lease", flag.ContinueOnError)
	endpoint := flags.String("engine-endpoint", "", "launching owner's held Docker endpoint")
	docker := flags.String("docker", "docker", "container runtime binary")
	descriptor := flags.Int("lease-fd", -1, "inherited private creation journal")
	check := flags.String("lease-check", "", "private creation journal path")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || !ownerEngineEndpoint(*endpoint) {
		return errors.New("owned cleanup endpoint required")
	}
	socket := filepath.Join(filepath.Dir(*check), delegates.ControlSocketBasename)
	lease, err := openOwnerLeaseMode(*descriptor, *check, socket, true)
	if err != nil {
		return err
	}
	defer lease.file.Close()
	info, err := lease.file.Stat()
	if err != nil {
		return err
	}
	if info.Size() == 0 {
		// Before any durable intent there is no creation call. The C owner has
		// joined the producer; current named custody authenticates this blank file.
		duplicate, err := syscall.Dup(int(lease.file.Fd()))
		if err != nil {
			return err
		}
		named, err := openOwnerLease(duplicate, *check, socket)
		if err != nil {
			return err
		}
		named.file.Close()
		return emitOwnerCleanup("")
	}
	if err = lease.read(); err != nil {
		return err
	}
	if lease.engine != *endpoint {
		return errors.New("creation lease daemon differs")
	}
	request := *lease.request
	var control, helper string
	for _, mount := range request.Spec.Mounts {
		if mount.Kind == delegates.SandboxControlSocket {
			control = mount.Source
		}
		if mount.Kind == delegates.SandboxOwnerHelper {
			helper = mount.Source
		}
	}
	if control != delegates.TranslateMountPath(socket, request.MountTable) || helper != delegates.TranslateMountPath(filepath.Join(filepath.Dir(*check), "owner-io"), request.MountTable) {
		return errors.New("journal private source scope differs")
	}

	pinned := func(ctx context.Context, binary string, args ...string) (string, error) {
		return run(ctx, binary, append([]string{"--host", *endpoint}, args...)...)
	}
	lease.recovering = true
	runner := delegates.ContainerRunner{Docker: *docker, Run: pinned, MountTable: request.MountTable, RequireImmutableIdentity: true, Journal: lease}
	id, err := runner.CleanupOwned(ctx, request, lease.id)
	if err != nil {
		return err
	}
	if !lease.removed {
		if err = lease.append(ownerLeaseRecord{Kind: "removed", ID: id}); err != nil {
			return err
		}
	}
	return emitOwnerCleanup(id)
}

func emitOwnerCleanup(id string) error {
	encoded, _ := json.Marshal(struct {
		Schema      string `json:"schema"`
		ID          string `json:"container_id,omitempty"`
		Removed     bool   `json:"removed"`
		NoContainer bool   `json:"no_container,omitempty"`
	}{"aimee.container-cleanup.v1", id, id != "", id == ""})
	_, err := fmt.Fprintln(os.Stdout, string(encoded))
	return err
}
