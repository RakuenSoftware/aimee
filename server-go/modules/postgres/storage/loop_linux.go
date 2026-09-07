//go:build linux

package storage

import (
	"errors"
	"fmt"
	"os"

	"golang.org/x/sys/unix"
)

// Containers do not run udev. Allocate just the loop node this volume needs,
// configure it atomically, and keep AUTOCLEAR enabled. A mapping that survives
// an abrupt container exit releases its loop when the next owner closes it.
func (v *Volume) attachLoop() error {
	control, err := unix.Open("/dev/loop-control", unix.O_RDWR|unix.O_CLOEXEC, 0)
	if err != nil {
		return errors.New("postgres storage: loop control unavailable")
	}
	defer unix.Close(control)
	backing, err := unix.Open(v.image(), unix.O_RDWR|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0)
	if err != nil {
		return errors.New("postgres storage: ciphertext unavailable")
	}
	defer unix.Close(backing)
	for attempt := 0; attempt < 16; attempt++ {
		number, err := unix.IoctlRetInt(control, unix.LOOP_CTL_GET_FREE)
		if err != nil || number < 0 {
			return errors.New("postgres storage: no loop device available")
		}
		path := fmt.Sprintf("/dev/loop%d", number)
		if err := unix.Mknod(path, unix.S_IFBLK|0600, int(unix.Mkdev(7, uint32(number)))); err != nil && !errors.Is(err, unix.EEXIST) {
			return errors.New("postgres storage: loop device creation denied")
		}
		var st unix.Stat_t
		if unix.Lstat(path, &st) != nil || st.Mode&unix.S_IFMT != unix.S_IFBLK || unix.Major(st.Rdev) != 7 || unix.Minor(st.Rdev) != uint32(number) {
			return errors.New("postgres storage: unexpected loop device")
		}
		fd, err := unix.Open(path, unix.O_RDWR|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0)
		if err != nil {
			return errors.New("postgres storage: loop device access denied")
		}
		config := unix.LoopConfig{Fd: uint32(backing), Info: unix.LoopInfo64{Flags: unix.LO_FLAGS_AUTOCLEAR}}
		if err := unix.IoctlLoopConfigure(fd, &config); err != nil {
			unix.Close(fd)
			if errors.Is(err, unix.EBUSY) {
				continue
			}
			return errors.New("postgres storage: atomic loop configuration unavailable")
		}
		v.loop = os.NewFile(uintptr(fd), path)
		return nil
	}
	return errors.New("postgres storage: loop allocation contention")
}
