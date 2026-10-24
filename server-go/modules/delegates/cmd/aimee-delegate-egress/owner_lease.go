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

const ownerLeaseSchema = "aimee.container-lease.v2"
const ordinaryLeaseSchema = "aimee.container-lease.v3"
const ordinaryResourceMode = "ordinary-container"
const ownerLeaseLegacySchema = "aimee.container-lease.v1"
const ownerLeaseMax = (1 << 20) + 4096

type ownerSourceIdentity struct {
	Dev   string `json:"dev"`
	Inode string `json:"inode"`
}
type ownerSourceIdentities struct {
	Socket ownerSourceIdentity `json:"socket,omitzero"`
	Helper ownerSourceIdentity `json:"helper,omitzero"`
	Parent ownerSourceIdentity `json:"parent"`
}

func sourceIdentity(info os.FileInfo) ownerSourceIdentity {
	stat := info.Sys().(*syscall.Stat_t)
	return ownerSourceIdentity{strconv.FormatUint(uint64(stat.Dev), 10), strconv.FormatUint(uint64(stat.Ino), 10)}
}
func (identity ownerSourceIdentity) valid() bool {
	dev, err := strconv.ParseUint(identity.Dev, 10, 64)
	inode, failure := strconv.ParseUint(identity.Inode, 10, 64)
	return err == nil && failure == nil && inode != 0 && strconv.FormatUint(dev, 10) == identity.Dev && strconv.FormatUint(inode, 10) == identity.Inode
}
func (identities *ownerSourceIdentities) valid() bool {
	return identities != nil && identities.Socket.valid() && identities.Helper.valid() && identities.Parent.valid()
}
func (identities *ownerSourceIdentities) validOrdinary() bool {
	return identities != nil && identities.Parent.valid() && identities.Socket == (ownerSourceIdentity{}) && identities.Helper == (ownerSourceIdentity{})
}

// Docker defines SystemInfo.ID as the daemon identifier. Its representation is
// opaque; never substitute a hostname, socket spelling, or assumed UUID format.
func ownerDaemonID(ctx context.Context, run delegates.CommandRunner, docker string) (string, error) {
	output, err := run(ctx, docker, "info", "--format", "{{json .ID}}")
	var id string
	if err != nil || len(output) > 4096 || json.Unmarshal([]byte(output), &id) != nil || strings.TrimSpace(id) == "" {
		return "", errors.New("actual Docker daemon identity unavailable")
	}
	return id, nil
}

type ownerLeaseRecord struct {
	ResourceMode string                         `json:"resource_mode,omitempty"`
	DaemonID     string                         `json:"daemon_id,omitempty"`
	Artifacts    *ownerSourceIdentities         `json:"source_identities,omitempty"`
	Schema       string                         `json:"schema"`
	Kind         string                         `json:"kind"`
	Engine       string                         `json:"engine_endpoint,omitempty"`
	HelperSHA    string                         `json:"helper_sha256,omitempty"`
	Startup      string                         `json:"trusted_startup,omitempty"`
	Request      *delegates.DockerCreateRequest `json:"request,omitempty"`
	LeaseDev     string                         `json:"lease_dev,omitempty"`
	LeaseInode   string                         `json:"lease_inode,omitempty"`
	ID           string                         `json:"container_id,omitempty"`
}
type ownerLease struct {
	check             string
	resourceMode      string
	schema, daemonID  string
	artifacts         *ownerSourceIdentities
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
	if filepath.Dir(check) != filepath.Dir(socket) {
		return nil, errors.New("private creation lease scope differs")
	}
	return openJournal(fd, check, retained)
}
func openJournal(fd int, check string, retained bool) (*ownerLease, error) {
	if fd < 3 || !filepath.IsAbs(check) || filepath.Clean(check) != check || filepath.Base(check) != "lease.jsonl" {
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
	if err := syscall.Flock(fd, syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		file.Close()
		return nil, errors.New("creation lease has another live owner")
	}
	return &ownerLease{file: file, schema: ownerLeaseSchema, check: check}, nil
}
func (lease *ownerLease) append(record ownerLeaseRecord) error {
	record.Schema = lease.schema
	if record.Schema == "" {
		record.Schema = ownerLeaseSchema
	}
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
	if err != nil || info.Size() != 0 || strings.TrimSpace(lease.daemonID) == "" ||
		(lease.resourceMode == "" && !lease.artifacts.valid()) || (lease.resourceMode != "" && (lease.resourceMode != ordinaryResourceMode || !lease.artifacts.validOrdinary() || request.LeaseNonce == "" || request.Startup == nil)) {
		return errors.New("fresh creation lease must be empty")
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return errors.New("creation lease source identity unavailable")
	}
	if err := lease.refuseSourceExposure(request); err != nil {
		return err
	}

	startup := "owner-io-idle-guard-v1"
	if lease.resourceMode == ordinaryResourceMode {
		startup = "image-default-v1"
	}
	if err := lease.append(ownerLeaseRecord{Kind: "intent", ResourceMode: lease.resourceMode, DaemonID: lease.daemonID, Artifacts: lease.artifacts, Engine: lease.engine, HelperSHA: lease.helperSHA, Startup: startup, Request: &request, LeaseDev: strconv.FormatUint(uint64(stat.Dev), 10), LeaseInode: strconv.FormatUint(uint64(stat.Ino), 10)}); err != nil {
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
		if decoder.Decode(&record) != nil || decoder.Decode(new(any)) != io.EOF || (record.Schema != ownerLeaseSchema && record.Schema != ownerLeaseLegacySchema && record.Schema != ordinaryLeaseSchema) {
			return errors.New("invalid complete creation lease record")
		}
		if index == 0 {
			lease.schema = record.Schema

			ordinary := record.Schema == ordinaryLeaseSchema
			validSource := (record.Schema == ownerLeaseSchema && record.Artifacts.valid() && record.ResourceMode == "") || (ordinary && record.ResourceMode == ordinaryResourceMode && record.Artifacts.validOrdinary())
			if record.Schema != ownerLeaseLegacySchema && (!validSource || strings.TrimSpace(record.DaemonID) == "" || len(record.DaemonID) > 4096) {
				return errors.New("durable daemon/source identity unavailable")
			}
			if record.Schema == ownerLeaseLegacySchema && (record.DaemonID != "" || record.Artifacts != nil || record.ResourceMode != "") {
				return errors.New("legacy source identity differs")
			}
			lease.daemonID, lease.artifacts, lease.resourceMode = record.DaemonID, record.Artifacts, record.ResourceMode
			expectedStartup := "owner-io-idle-guard-v1"
			if ordinary {
				expectedStartup = "image-default-v1"
			}
			if record.Kind != "intent" || record.Request == nil || record.Engine == "" || record.Startup != expectedStartup || record.ID != "" ||
				(!ordinary && (len(record.HelperSHA) != 64 || strings.Trim(record.HelperSHA, "0123456789abcdef") != "")) ||
				(ordinary && (record.HelperSHA != "" || record.Request.LeaseNonce == "" || record.Request.Startup == nil)) {
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
			if record.ResourceMode != "" || record.Schema != lease.schema || record.DaemonID != "" || record.Artifacts != nil || record.Request != nil || record.Engine != "" || record.HelperSHA != "" || record.Startup != "" || record.LeaseDev != "" || record.LeaseInode != "" || len(record.ID) != 64 || strings.Trim(record.ID, "0123456789abcdef") != "" {
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
	recoverOwner := flags.Bool("recover", false, "recover an unlocked v2 journal through a requalified daemon")
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
		if *recoverOwner {
			return errors.New("no durable recovery intent")
		}
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
		return emitOwnerCleanup("", nil, "")
	}
	if err = lease.read(); err != nil {
		return err
	}
	pinned := func(ctx context.Context, binary string, args ...string) (string, error) {
		return run(ctx, binary, append([]string{"--host", *endpoint}, args...)...)
	}
	if !*recoverOwner && lease.engine != *endpoint {
		return errors.New("creation lease endpoint differs")
	}
	if *recoverOwner && ((lease.schema != ownerLeaseSchema && lease.schema != ordinaryLeaseSchema) || info.Size() != lease.validEnd) {
		return errors.New("complete v2 recovery intent required")
	}
	if lease.schema == ownerLeaseSchema || lease.schema == ordinaryLeaseSchema {
		actual, err := ownerDaemonID(ctx, pinned, *docker)
		if err != nil || actual != lease.daemonID {
			return errors.New("creation lease daemon identity differs")
		}
	} else if lease.engine != *endpoint {
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
	if lease.resourceMode == ordinaryResourceMode {
		if request.LeaseNonce != filepath.Base(filepath.Dir(*check)) {
			return errors.New("ordinary journal scope differs")
		}
	} else if control != delegates.TranslateMountPath(socket, request.MountTable) || helper != delegates.TranslateMountPath(filepath.Join(filepath.Dir(*check), "owner-io"), request.MountTable) {
		return errors.New("journal private source scope differs")
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
	return emitOwnerCleanup(id, lease.artifacts, lease.resourceMode)
}

func emitOwnerCleanup(id string, artifacts *ownerSourceIdentities, mode string) error {
	encoded, _ := json.Marshal(struct {
		ResourceMode string                 `json:"resource_mode,omitempty"`
		Artifacts    *ownerSourceIdentities `json:"source_identities,omitempty"`
		Schema       string                 `json:"schema"`
		ID           string                 `json:"container_id,omitempty"`
		Removed      bool                   `json:"removed"`
		NoContainer  bool                   `json:"no_container,omitempty"`
	}{mode, artifacts, "aimee.container-cleanup.v1", id, id != "", id == ""})
	_, err := fmt.Fprintln(os.Stdout, string(encoded))
	return err
}
