package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/modules/delegates"
	"golang.org/x/sys/unix"
)

type kernelMount struct {
	id          uint64
	device      string
	root, point string
}
type physicalDirectory struct {
	namespace    string
	mountID      uint64
	device, path string
	stat         unix.Stat_t
}

func boundedKernelFile(path string, limit int) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	bytes, err := io.ReadAll(io.LimitReader(file, int64(limit+1)))
	if err != nil || len(bytes) > limit {
		return nil, errors.New("kernel mount evidence unavailable")
	}
	return bytes, nil
}
func kernelPath(value string) (string, error) {
	var out strings.Builder
	for i := 0; i < len(value); i++ {
		if value[i] != '\\' {
			out.WriteByte(value[i])
			continue
		}
		if i+3 >= len(value) {
			return "", errors.New("incomplete mount path escape")
		}
		escape := value[i+1 : i+4]
		decoded, ok := map[string]byte{"040": ' ', "011": '\t', "012": '\n', "134": '\\'}[escape]
		if !ok {
			return "", errors.New("unknown mount path escape")
		}
		out.WriteByte(decoded)
		i += 3
	}
	path := out.String()
	if !filepath.IsAbs(path) || filepath.Clean(path) != path || strings.IndexByte(path, 0) >= 0 {
		return "", errors.New("ambiguous mount path")
	}
	return path, nil
}
func kernelMounts(encoded []byte) (map[uint64]kernelMount, error) {
	result := map[uint64]kernelMount{}
	for _, line := range strings.Split(strings.TrimSuffix(string(encoded), "\n"), "\n") {
		prefix, _, ok := strings.Cut(line, " - ")
		fields := strings.Fields(prefix)
		if !ok || len(fields) < 6 {
			return nil, errors.New("invalid mount record")
		}
		id, err := strconv.ParseUint(fields[0], 10, 64)
		if err != nil || id == 0 || strconv.FormatUint(id, 10) != fields[0] {
			return nil, errors.New("invalid mount identity")
		}
		if _, exists := result[id]; exists {
			return nil, errors.New("duplicate mount identity")
		}
		root, err := kernelPath(fields[3])
		if err != nil {
			return nil, err
		}
		point, err := kernelPath(fields[4])
		if err != nil {
			return nil, err
		}
		result[id] = kernelMount{id, fields[2], root, point}
	}
	return result, nil
}
func physicalFD(fd int, mounts map[uint64]kernelMount) (physicalDirectory, error) {
	var result physicalDirectory
	if err := unix.Fstat(fd, &result.stat); err != nil || result.stat.Mode&unix.S_IFMT != unix.S_IFDIR {
		return result, errors.New("held directory identity unavailable")
	}
	encoded, err := boundedKernelFile(fmt.Sprintf("/proc/self/fdinfo/%d", fd), 16384)
	if err != nil {
		return result, err
	}
	var mountID, inode uint64
	for _, line := range strings.Split(string(encoded), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		if fields[0] == "mnt_id:" {
			if mountID != 0 {
				return result, errors.New("ambiguous mount ID")
			}
			mountID, err = strconv.ParseUint(fields[1], 10, 64)
		}
		if fields[0] == "ino:" {
			if inode != 0 {
				return result, errors.New("ambiguous inode")
			}
			inode, err = strconv.ParseUint(fields[1], 10, 64)
		}
		if err != nil {
			return result, err
		}
	}
	mount, ok := mounts[mountID]
	device := fmt.Sprintf("%d:%d", unix.Major(uint64(result.stat.Dev)), unix.Minor(uint64(result.stat.Dev)))
	if !ok || inode != result.stat.Ino || mount.device != device {
		return result, errors.New("held directory/mount identity differs")
	}
	path, err := os.Readlink(fmt.Sprintf("/proc/self/fd/%d", fd))
	if err != nil || !filepath.IsAbs(path) || filepath.Clean(path) != path || strings.HasSuffix(path, " (deleted)") {
		return result, errors.New("ambiguous held directory spelling")
	}
	relative, err := filepath.Rel(mount.point, path)
	if err != nil || relative == ".." || strings.HasPrefix(relative, "../") {
		return result, errors.New("directory outside recorded mount")
	}
	check, err := unix.Open(path, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return result, err
	}
	defer unix.Close(check)
	var named unix.Stat_t
	if unix.Fstat(check, &named) != nil || named.Dev != result.stat.Dev || named.Ino != result.stat.Ino {
		return result, errors.New("held directory spelling changed")
	}
	result.device = device
	result.namespace = path
	result.mountID = mountID
	result.path = filepath.Join(mount.root, relative)
	return result, nil
}

// A bind can cut namespace '..' ancestry. Kernel mnt_id joins recover its actual
// filesystem root; a matching inode reached beneath the source proves exposure.
// Source and owner stay open throughout. Uncheckable/ambiguous evidence refuses
// before durable intent, image startup, or opaque code can run.
func (lease *ownerLease) refuseSourceExposure(request delegates.DockerCreateRequest) error {
	parent, err := unix.Open(filepath.Dir(lease.check), unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return err
	}
	defer unix.Close(parent)
	journal, err := unix.Openat(parent, "lease.jsonl", unix.O_RDONLY|unix.O_NOFOLLOW|unix.O_NONBLOCK|unix.O_CLOEXEC, 0)
	if err != nil {
		return err
	}
	defer unix.Close(journal)
	var held, named unix.Stat_t
	if unix.Fstat(int(lease.file.Fd()), &held) != nil || unix.Fstat(journal, &named) != nil || held.Dev != named.Dev || held.Ino != named.Ino {
		return errors.New("owner parent does not hold the creation journal")
	}
	encoded, err := boundedKernelFile("/proc/self/mountinfo", 8<<20)
	if err != nil {
		return err
	}
	mounts, err := kernelMounts(encoded)
	if err != nil {
		return err
	}
	owner, err := physicalFD(parent, mounts)
	if err != nil {
		return err
	}
	for _, mount := range request.Spec.Mounts {
		if mount.Kind != delegates.SandboxWorkspace && mount.Kind != delegates.SandboxGitMetadata {
			continue
		}
		source, err := unix.Open(mount.Source, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
		if err != nil {
			return fmt.Errorf("source containment unavailable: %w", err)
		}
		actual, failure := physicalFD(source, mounts)
		if failure != nil {
			unix.Close(source)
			return failure
		}
		failure = sourceExposesOwner(source, actual, owner)
		if failure != nil {
			unix.Close(source)
			return failure
		}
		// Docker's ordinary directory binds are recursive. A different-device
		// descendant can expose the owner even when the source root is disjoint.
		for _, child := range mounts {
			relative, err := filepath.Rel(actual.namespace, child.point)
			if err != nil || relative == "." || relative == ".." || strings.HasPrefix(relative, "../") || child.device != owner.device {
				continue
			}
			nested, err := unix.Openat(source, relative, unix.O_RDONLY|unix.O_NONBLOCK|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
			if err != nil {
				unix.Close(source)
				return errors.New("recursive source mount containment unavailable")
			}
			var status unix.Stat_t
			if unix.Fstat(nested, &status) != nil {
				unix.Close(nested)
				unix.Close(source)
				return errors.New("recursive source identity unavailable")
			}
			if status.Mode&unix.S_IFMT != unix.S_IFDIR {
				unix.Close(nested)
				continue
			}
			mapped, err := physicalFD(nested, mounts)
			if err != nil {
				unix.Close(nested)
				unix.Close(source)
				return err
			}
			if mapped.mountID == child.id {
				err = sourceExposesOwner(nested, mapped, owner)
			}
			unix.Close(nested)
			if err != nil {
				unix.Close(source)
				return err
			}
		}

		unix.Close(source)
	}
	return nil
}

func sourceExposesOwner(source int, actual, owner physicalDirectory) error {
	relative, err := filepath.Rel(actual.path, owner.path)
	if actual.device != owner.device || err != nil || relative == ".." || strings.HasPrefix(relative, "../") {
		return nil
	}
	reached, err := unix.Openat(source, relative, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return errors.New("source/private owner containment is ambiguous")
	}
	defer unix.Close(reached)
	var candidate unix.Stat_t
	if unix.Fstat(reached, &candidate) != nil || candidate.Dev != owner.stat.Dev || candidate.Ino != owner.stat.Ino {
		return errors.New("source/private owner containment is ambiguous")
	}
	return errors.New("source mount exposes private resource owner")
}

// These fields are projected by the C producer from one pinned-daemon inspect,
// matched to that same inspect's actual mounts. No propagation guess is used.
type controllerBind struct {
	Source       string `json:"source"`
	Target       string `json:"target"`
	NonRecursive bool   `json:"non_recursive"`
}

func verifyControllerBinds(encoded, mapping, check string, spec delegates.SandboxSpec) error {
	if encoded == "" {
		return nil
	} // Host-native producer has no controller bind layer.
	if len(encoded) > 16384 {
		return errors.New("controller bind evidence exceeds bound")
	}
	var binds []struct {
		Source       string `json:"source"`
		Target       string `json:"target"`
		NonRecursive *bool  `json:"non_recursive"`
	}
	decoder := json.NewDecoder(strings.NewReader(encoded))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&binds) != nil || decoder.Decode(new(any)) != io.EOF {
		return errors.New("invalid controller bind evidence")
	}
	paths := []string{filepath.Dir(check)}
	for _, mount := range spec.Mounts {
		if mount.Kind == delegates.SandboxWorkspace || mount.Kind == delegates.SandboxGitMetadata {
			paths = append(paths, mount.Source)
		}
	}
	for _, bind := range binds {
		if bind.NonRecursive == nil {
			return errors.New("ambiguous controller recursion evidence")
		}
		if !filepath.IsAbs(bind.Source) || filepath.Clean(bind.Source) != bind.Source || !filepath.IsAbs(bind.Target) || filepath.Clean(bind.Target) != bind.Target || delegates.TranslateMountPath(bind.Target, mapping) != bind.Source {
			return errors.New("controller bind does not match actual mount translation")
		}
		if !*bind.NonRecursive {
			continue
		}
		info, err := os.Stat(bind.Target)
		if err != nil {
			return errors.New("nonrecursive controller layout unavailable")
		}
		if !info.IsDir() {
			continue
		}
		target, err := filepath.EvalSymlinks(bind.Target)
		if err != nil {
			return err
		}
		for _, path := range paths {
			real, err := filepath.EvalSymlinks(path)
			if err != nil {
				return errors.New("controller/source identity unavailable")
			}
			relative, err := filepath.Rel(target, real)
			if err != nil {
				return err
			}
			if relative != ".." && !strings.HasPrefix(relative, "../") {
				return errors.New("unsupported nonrecursive controller bind can hide host submounts")
			}
		}
	}
	return nil
}
