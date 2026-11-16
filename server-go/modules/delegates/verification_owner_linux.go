//go:build linux

package delegates

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"golang.org/x/sys/unix"
	"io"
	"os"
	"path/filepath"
)

const verifierKindLabel = "org.aimee.private-tests.kind"
const verifierOwnerLabel = "org.aimee.private-tests.owner"
const verifierWorkerLabel = "org.aimee.private-tests.worker"
const verifierKind = "configured-v1"

// PrivateWorkerLease owns only resource-lifecycle metadata. Its lock remains
// held until worker removal finishes; different workers never share a lock.
type PrivateWorkerLease struct {
	directory     *os.File
	lock          *os.File
	owner, worker string
}

func randomWorkerToken() (string, error) {
	var raw [32]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(raw[:]), nil
}
func privateOwnerDirectory(path string) (*os.File, error) {
	if !filepath.IsAbs(path) || filepath.Clean(path) != path {
		return nil, errors.New("absolute verifier owner directory required")
	}
	if err := os.Mkdir(path, 0700); err != nil && !os.IsExist(err) {
		return nil, err
	}
	fd, err := unix.Open(path, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	directory := os.NewFile(uintptr(fd), path)
	info, err := directory.Stat()
	if err != nil {
		directory.Close()
		return nil, err
	}
	var actual unix.Stat_t
	if unix.Fstat(fd, &actual) != nil || actual.Uid != uint32(os.Geteuid()) || info.Mode().Perm()&0077 != 0 {
		directory.Close()
		return nil, errors.New("unsafe verifier owner directory")
	}
	return directory, nil
}
func privateOwnerFile(directory *os.File, name string, create bool) (*os.File, error) {
	flags := unix.O_RDWR | unix.O_NOFOLLOW | unix.O_CLOEXEC
	if create {
		flags |= unix.O_CREAT
	}
	fd, err := unix.Openat(int(directory.Fd()), name, flags, 0600)
	if err != nil {
		return nil, err
	}
	var st unix.Stat_t
	if unix.Fstat(fd, &st) != nil || st.Uid != uint32(os.Geteuid()) || st.Mode&unix.S_IFMT != unix.S_IFREG || st.Mode&0077 != 0 || st.Nlink != 1 {
		unix.Close(fd)
		return nil, errors.New("unsafe verifier owner file")
	}
	return os.NewFile(uintptr(fd), name), nil
}
func ownerToken(directory *os.File) (string, error) {
	file, err := privateOwnerFile(directory, "owner", true)
	if err != nil {
		return "", err
	}
	defer file.Close()
	if err = unix.Flock(int(file.Fd()), unix.LOCK_EX); err != nil {
		return "", err
	}
	defer unix.Flock(int(file.Fd()), unix.LOCK_UN)
	data, err := io.ReadAll(io.LimitReader(file, 66))
	if err != nil {
		return "", err
	}
	if len(data) == 0 {
		token, err := randomWorkerToken()
		if err != nil {
			return "", err
		}
		if _, err = file.WriteString(token + "\n"); err != nil {
			return "", err
		}
		if err = file.Sync(); err != nil {
			return "", err
		}
		if err = directory.Sync(); err != nil {
			return "", err
		}
		return token, nil
	}
	if len(data) != 65 || data[64] != '\n' || !immutableContainerID(string(data[:64])) {
		return "", errors.New("invalid verifier owner token")
	}
	return string(data[:64]), nil
}
func AcquirePrivateWorkerLease(path string) (*PrivateWorkerLease, error) {
	directory, err := privateOwnerDirectory(path)
	if err != nil {
		return nil, err
	}
	owner, err := ownerToken(directory)
	if err != nil {
		directory.Close()
		return nil, err
	}
	worker, err := randomWorkerToken()
	if err != nil {
		directory.Close()
		return nil, err
	}
	lock, err := privateOwnerFile(directory, worker+".lock", true)
	if err != nil {
		directory.Close()
		return nil, err
	}
	if err = unix.Flock(int(lock.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		lock.Close()
		directory.Close()
		return nil, err
	}
	if err = directory.Sync(); err != nil {
		lock.Close()
		directory.Close()
		return nil, err
	}
	return &PrivateWorkerLease{directory: directory, lock: lock, owner: owner, worker: worker}, nil
}
func (lease *PrivateWorkerLease) Labels() map[string]string {
	return map[string]string{verifierKindLabel: verifierKind, verifierOwnerLabel: lease.owner, verifierWorkerLabel: lease.worker}
}
func (lease *PrivateWorkerLease) Close(removed bool) {
	if removed {
		_ = unix.Unlinkat(int(lease.directory.Fd()), lease.worker+".lock", 0)
	}
	lease.lock.Close()
	lease.directory.Close()
}

// ReapPrivateWorkers cannot touch a live helper or another installation. List
// filtering is only a candidate selector; actual inspected labels are decisive.
func ReapPrivateWorkers(ctx context.Context, path string, engine *egress.DockerEngine) (int, error) {
	directory, err := privateOwnerDirectory(path)
	if err != nil {
		return 0, err
	}
	defer directory.Close()
	owner, err := ownerToken(directory)
	if err != nil {
		return 0, err
	}
	ids, err := engine.ListLabeledContainers(ctx, map[string]string{verifierKindLabel: verifierKind, verifierOwnerLabel: owner})
	if err != nil {
		return 0, err
	}
	removed := 0
	for _, id := range ids {
		raw, err := engine.InspectContainer(ctx, id)
		if err != nil {
			return removed, err
		}
		var actual struct {
			ID     string `json:"Id"`
			Config struct {
				Labels map[string]string `json:"Labels"`
			} `json:"Config"`
		}
		if json.Unmarshal(raw, &actual) != nil || actual.ID != id || actual.Config.Labels[verifierKindLabel] != verifierKind || actual.Config.Labels[verifierOwnerLabel] != owner {
			continue
		}
		worker := actual.Config.Labels[verifierWorkerLabel]
		if !immutableContainerID(worker) {
			continue
		}
		lock, err := privateOwnerFile(directory, worker+".lock", false)
		if err != nil {
			continue
		}
		if unix.Flock(int(lock.Fd()), unix.LOCK_EX|unix.LOCK_NB) != nil {
			lock.Close()
			continue
		}
		// Re-inspect while holding custody; Docker IDs cannot be reused.
		again, err := engine.InspectContainer(ctx, id)
		var checked struct {
			ID     string `json:"Id"`
			Config struct {
				Labels map[string]string `json:"Labels"`
			} `json:"Config"`
		}
		if err == nil && json.Unmarshal(again, &checked) == nil && checked.ID == id && checked.Config.Labels[verifierKindLabel] == verifierKind && checked.Config.Labels[verifierOwnerLabel] == owner && checked.Config.Labels[verifierWorkerLabel] == worker {
			err = engine.Remove(ctx, id)
			if err == nil {
				removed++
				_ = unix.Unlinkat(int(directory.Fd()), worker+".lock", 0)
			}
		}
		lock.Close()
		if err != nil {
			return removed, err
		}
	}
	return removed, nil
}
