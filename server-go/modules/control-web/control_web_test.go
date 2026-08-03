package controlweb

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/control-web/policy"
)

func authorizationWire(target uint32, method, path string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], target)
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(method)))
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(path)))
	copy(request[requestMethodOff:requestPathOff], method)
	copy(request[requestPathOff:], path)
	return request
}

func responseAllowed(t *testing.T, response []byte) bool {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(response[8:12]) > 1 ||
		binary.LittleEndian.Uint32(response[12:16]) != 0 {
		t.Fatalf("invalid response %x", response)
	}
	return binary.LittleEndian.Uint32(response[8:12]) == 1
}

func TestProxyRouteAuthorizationParity(t *testing.T) {
	tests := []struct {
		name         string
		target       uint32
		method, path string
		want         bool
	}{
		{"console exact", TargetConsoleAdmin, "GET", "/v1/console/overview", true},
		{"console wildcard", TargetConsoleAdmin, "POST", "/v1/enrollments/abc/revoke", true},
		{"console trailing slash", TargetConsoleAdmin, "GET", "/v1/enrollments/", true},
		{"console wrong method", TargetConsoleAdmin, "DELETE", "/v1/enrollments/abc/revoke", false},
		{"console encoded", TargetConsoleAdmin, "GET", "/v1/%65nrollments", false},
		{"console fleet separation", TargetConsoleAdmin, "GET", "/v1/servers/s1/health", false},
		{"fleet exact", TargetFleet, "GET", "/v1/servers/s1/health", true},
		{"fleet mutation", TargetFleet, "POST", "/v1/servers/s1/actions", true},
		{"fleet trailing slash", TargetFleet, "GET", "/v1/servers/", false},
		{"fleet wrong method", TargetFleet, "POST", "/v1/servers/s1/config", false},
		{"empty", TargetConsoleAdmin, "", "", false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			shared := policy.ConsoleAdminAllows(test.method, test.path)
			if test.target == TargetFleet {
				shared = policy.FleetAllows(test.method, test.path)
			}
			if shared != test.want {
				t.Fatalf("shared policy = %v, want %v", shared, test.want)
			}
			response, status := Handle(bus.ModuleInvocation{StageID: StageAuthorize},
				authorizationWire(test.target, test.method, test.path))
			if status != bus.ModuleStatusOK {
				t.Fatalf("handler status = %d", status)
			}
			if got := responseAllowed(t, response); got != test.want {
				t.Fatalf("response allowed = %v, want %v", got, test.want)
			}
		})
	}
}

func TestProxyRouteAuthorizationRejectsMalformedWire(t *testing.T) {
	valid := func() []byte {
		return authorizationWire(TargetConsoleAdmin, "GET", "/v1/enrollments")
	}
	tests := [][]byte{nil, valid()[:requestLen-1]}
	badMagic := valid()
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := valid()
	badVersion[4]++
	tests = append(tests, badVersion)
	badTarget := valid()
	binary.LittleEndian.PutUint32(badTarget[8:12], ^uint32(0))
	tests = append(tests, badTarget)
	badMethodLength := valid()
	binary.LittleEndian.PutUint32(badMethodLength[12:16], ^uint32(0))
	tests = append(tests, badMethodLength)
	badPathLength := valid()
	binary.LittleEndian.PutUint32(badPathLength[16:20], ^uint32(0))
	tests = append(tests, badPathLength)
	reserved := valid()
	reserved[20] = 1
	tests = append(tests, reserved)
	embeddedZero := valid()
	embeddedZero[requestPathOff+1] = 0
	tests = append(tests, embeddedZero)
	methodPadding := valid()
	methodPadding[requestMethodOff+len("GET")] = 1
	tests = append(tests, methodPadding)
	pathPadding := valid()
	pathPadding[requestPathOff+len("/v1/enrollments")] = 1
	tests = append(tests, pathPadding)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageAuthorize}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageAuthorize + 1}, valid()); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestProxyRouteAuthorizationWireBoundsAndCancellation(t *testing.T) {
	maximum := authorizationWire(TargetConsoleAdmin, strings.Repeat("M", methodMax),
		"/"+strings.Repeat("p", pathMax-1))
	if _, status := Handle(bus.ModuleInvocation{StageID: StageAuthorize}, maximum); status != bus.ModuleStatusOK {
		t.Fatalf("maximum canonical wire status = %d", status)
	}
	invocation := bus.ModuleInvocation{StageID: StageAuthorize, DeadlineNS: 1}
	valid := authorizationWire(TargetFleet, "GET", "/v1/servers")
	if _, status := Handle(invocation, valid); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
