package main

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

// The launching C owner holds this daemon socket inode for the entire lease.
// An explicit parent procfd keeps Docker's ambient context out of every command.
func ownerEngineEndpoint(endpoint string) bool {
	prefix := fmt.Sprintf("unix:///proc/%d/fd/", os.Getppid())
	if !strings.HasPrefix(endpoint, prefix) {
		return false
	}
	value := strings.TrimPrefix(endpoint, prefix)
	fd, err := strconv.Atoi(value)
	return err == nil && fd >= 3 && strconv.Itoa(fd) == value
}

func measuredOwnerHelper(check, source, expected, socket, mounts string) (*os.File, string, error) {
	if filepath.Base(check) != "owner-io" || filepath.Dir(check) != filepath.Dir(socket) || filepath.Clean(check) != check || !filepath.IsAbs(source) || filepath.Clean(source) != source || delegates.TranslateMountPath(check, mounts) != source || len(expected) != 64 || strings.Trim(expected, "0123456789abcdef") != "" {
		return nil, "", errors.New("measured helper must share the private control scope")
	}
	fd, err := syscall.Open(check, syscall.O_RDONLY|syscall.O_CLOEXEC|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return nil, "", err
	}
	file := os.NewFile(uintptr(fd), "owner-io")
	digest, err := verifyOwnerHelper(file, check, expected)
	if err != nil {
		file.Close()
		return nil, "", err
	}
	return file, digest, nil
}

func verifyOwnerHelper(file *os.File, check, expected string) (string, error) {
	info, err := file.Stat()
	if err != nil {
		return "", err
	}
	current, err := os.Lstat(check)
	if err != nil {
		return "", err
	}
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok || !info.Mode().IsRegular() || info.Mode().Perm() != 0555 || int(stat.Uid) != os.Geteuid() || stat.Nlink != 1 || !os.SameFile(info, current) || current.Mode()&os.ModeSymlink != 0 {
		return "", errors.New("private owner helper identity unavailable")
	}
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return "", err
	}
	hash := sha256.New()
	count, err := io.Copy(hash, io.LimitReader(file, (64<<20)+1))
	digest := hex.EncodeToString(hash.Sum(nil))
	if err != nil || count > 64<<20 || digest != expected {
		return "", errors.New("private owner helper measurement differs")
	}
	return digest, nil
}
