package bus

import (
	"encoding/binary"
	"errors"
	"testing"

	"golang.org/x/sys/unix"
)

func TestAttachAsCarriesClaimedIdentity(t *testing.T) {
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_SEQPACKET, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fds[0])
	defer unix.Close(fds[1])

	request := make(chan [attachReqBytes]byte, 1)
	serverErr := make(chan error, 1)
	go func() {
		var wire [attachReqBytes]byte
		n, _, _, _, recvErr := unix.Recvmsg(fds[1], wire[:], nil, 0)
		if recvErr != nil || n != len(wire) {
			if recvErr == nil {
				recvErr = ErrProtocol
			}
			serverErr <- recvErr
			return
		}
		request <- wire
		var reply [attachReplyBytes]byte
		binary.LittleEndian.PutUint32(reply[0:], attachReplyMagic)
		binary.LittleEndian.PutUint32(reply[4:], uint32(AttachDeniedPolicy))
		serverErr <- unix.Sendmsg(fds[1], reply[:], nil, nil, 0)
	}()

	client, attachErr := AttachAs(fds[0], 7, 23)
	if !errors.Is(attachErr, ErrDenied) || client == nil || client.Status != AttachDeniedPolicy {
		t.Fatalf("AttachAs = %#v, %v", client, attachErr)
	}
	wire := <-request
	if binary.LittleEndian.Uint32(wire[8:]) != 7 || binary.LittleEndian.Uint32(wire[12:]) != 23 {
		t.Fatalf("identity = (%d,%d), want (7,23)",
			binary.LittleEndian.Uint32(wire[8:]), binary.LittleEndian.Uint32(wire[12:]))
	}
	if err := <-serverErr; err != nil {
		t.Fatal(err)
	}
}
