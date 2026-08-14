package sandbox

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventProxyRequest uint32 = 10755
	StageProxyRequest uint32 = 3
	EventProxyAddress uint32 = 10756
	StageProxyAddress uint32 = 4

	ProxyRequestInvalid  = 0
	ProxyRequestAPI      = 1
	ProxyRequestConnect  = 2
	ProxyRequestAbsolute = 3

	proxyHostMax = 256
)

const DefaultPackageAllowlist = "deb.debian.org,security.debian.org,*.archive.ubuntu.com," +
	"security.ubuntu.com,registry.npmjs.org,pypi.org,files.pythonhosted.org"

type ProxyRequestPolicyRequest struct {
	Line      string  `json:"line"`
	Allowlist *string `json:"allowlist,omitempty"`
}

type ProxyRequestPolicyResponse struct {
	Kind        int    `json:"kind"`
	Host        string `json:"host"`
	Port        int    `json:"port"`
	Allowed     bool   `json:"allowed"`
	ForwardHead string `json:"forward_head,omitempty"`
}

type ProxyAddressPolicyRequest struct {
	IP string `json:"ip"`
}

type ProxyAddressPolicyResponse struct {
	Blocked bool `json:"blocked"`
}

func proxyPortAllowed(port int) bool {
	return port == 80 || port == 443
}

func ipv4Blocked(address uint32) bool {
	ranges := [...]struct {
		network uint32
		bits    uint
	}{
		{0x00000000, 8},  // this-network / unspecified
		{0x0a000000, 8},  // RFC1918
		{0x64400000, 10}, // CGNAT
		{0x7f000000, 8},  // loopback
		{0xa9fe0000, 16}, // link-local and cloud metadata
		{0xac100000, 12}, // RFC1918
		{0xc0000000, 24}, // IETF protocol assignments
		{0xc0000200, 24}, // TEST-NET-1
		{0xc0a80000, 16}, // RFC1918
		{0xc6120000, 15}, // benchmarking
		{0xc6336400, 24}, // TEST-NET-2
		{0xcb007100, 24}, // TEST-NET-3
		{0xe0000000, 4},  // multicast
		{0xf0000000, 4},  // reserved and broadcast
	}
	for _, blocked := range ranges {
		mask := ^uint32(0) << (32 - blocked.bits)
		if address&mask == blocked.network {
			return true
		}
	}
	return false
}

func proxyIPBlocked(ip net.IP) bool {
	if ip == nil {
		return true
	}
	if v4 := ip.To4(); v4 != nil {
		return ipv4Blocked(binary.BigEndian.Uint32(v4))
	}
	v6 := ip.To16()
	if v6 == nil {
		return true
	}

	// IPv6 transition mechanisms inherit the policy of the embedded IPv4
	// address. Otherwise private IPv4 can be smuggled through a public-looking
	// IPv6 result.
	compat := true
	for _, value := range v6[:12] {
		if value != 0 {
			compat = false
			break
		}
	}
	nat64 := []byte{0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0}
	if (compat && (v6[12]|v6[13]|v6[14]|v6[15]) != 0) || string(v6[:12]) == string(nat64) {
		return ipv4Blocked(binary.BigEndian.Uint32(v6[12:16]))
	}
	if v6[0] == 0x20 && v6[1] == 0x02 { // 6to4: 2002:V4:V4::
		return ipv4Blocked(binary.BigEndian.Uint32(v6[2:6]))
	}

	allZero := true
	for _, value := range v6[:15] {
		if value != 0 {
			allZero = false
			break
		}
	}
	if allZero && (v6[15] == 0 || v6[15] == 1) {
		return true
	}
	return v6[0]&0xfe == 0xfc || // unique-local
		(v6[0] == 0xfe && v6[1]&0xc0 == 0x80) || // link-local
		v6[0] == 0xff // multicast
}

func equalFoldASCII(left, right string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		a, b := left[index], right[index]
		if a >= 'A' && a <= 'Z' {
			a += 'a' - 'A'
		}
		if b >= 'A' && b <= 'Z' {
			b += 'a' - 'A'
		}
		if a != b {
			return false
		}
	}
	return true
}

func proxyHostAllowed(host, allowlist string) bool {
	if host == "" || allowlist == "" {
		return false
	}
	entries := strings.FieldsFunc(allowlist, func(value rune) bool {
		return value == ',' || value == ' ' || value == '\t' || value == '\n' || value == '\r'
	})
	for _, entry := range entries {
		if strings.HasPrefix(entry, "*.") {
			suffix := entry[2:]
			if suffix == "" || len(suffix) >= proxyHostMax {
				continue
			}
			if equalFoldASCII(host, suffix) ||
				(len(host) > len(suffix)+1 && host[len(host)-len(suffix)-1] == '.' &&
					equalFoldASCII(host[len(host)-len(suffix):], suffix)) {
				return true
			}
			continue
		}
		if equalFoldASCII(host, entry) {
			return true
		}
	}
	return false
}

func lowerASCII(value byte) byte {
	if value >= 'A' && value <= 'Z' {
		return value + ('a' - 'A')
	}
	return value
}

func copyProxyHost(value string) (string, bool) {
	if len(value) == 0 || len(value) >= proxyHostMax {
		return "", false
	}
	bracketed := value[0] == '['
	result := make([]byte, 0, len(value))
	for index := range value {
		current := value[index]
		if bracketed && (current == '[' || current == ']') {
			continue
		}
		switch current {
		case '/', ' ', '\r', '\n', '@', 0:
			return "", false
		}
		if len(result)+1 >= proxyHostMax {
			return "", false
		}
		result = append(result, lowerASCII(current))
	}
	return string(result), len(result) > 0
}

func parseProxyPort(value string) (int, bool) {
	if value == "" {
		return 0, false
	}
	port := 0
	for index := range value {
		if value[index] < '0' || value[index] > '9' {
			return 0, false
		}
		port = port*10 + int(value[index]-'0')
		if port > 65535 {
			return 0, false
		}
	}
	return port, port != 0
}

func classifyProxyRequestLine(line string) (int, string, int) {
	firstSpace := strings.IndexByte(line, ' ')
	if firstSpace <= 0 {
		return ProxyRequestInvalid, "", 0
	}
	targetStart := firstSpace + 1
	secondRelative := strings.IndexByte(line[targetStart:], ' ')
	if secondRelative <= 0 {
		return ProxyRequestInvalid, "", 0
	}
	secondSpace := targetStart + secondRelative
	target := line[targetStart:secondSpace]
	version := line[secondSpace+1:]
	if len(version) < 8 || version[:7] != "HTTP/1." || (version[7] != '0' && version[7] != '1') ||
		(len(version) > 8 && version[8] != 0 && version[8] != '\r' && version[8] != '\n' && version[8] != ' ') {
		return ProxyRequestInvalid, "", 0
	}

	method := line[:firstSpace]
	if method == "CONNECT" {
		colon := strings.LastIndexByte(target, ':')
		if colon <= 0 {
			return ProxyRequestInvalid, "", 0
		}
		port, ok := parseProxyPort(target[colon+1:])
		if !ok {
			return ProxyRequestInvalid, "", 0
		}
		host, ok := copyProxyHost(target[:colon])
		if !ok {
			return ProxyRequestInvalid, "", 0
		}
		return ProxyRequestConnect, host, port
	}

	if strings.HasPrefix(target, "/") {
		return ProxyRequestAPI, "", 0
	}
	if len(target) <= 7 || !strings.HasPrefix(target, "http://") {
		return ProxyRequestInvalid, "", 0
	}
	authority := target[7:]
	hostLen := 0
	for hostLen < len(authority) && authority[hostLen] != '/' && authority[hostLen] != ':' {
		hostLen++
	}
	port := 80
	if hostLen < len(authority) && authority[hostLen] == ':' {
		portEnd := hostLen + 1
		for portEnd < len(authority) && authority[portEnd] != '/' {
			portEnd++
		}
		var ok bool
		port, ok = parseProxyPort(authority[hostLen+1 : portEnd])
		if !ok {
			return ProxyRequestInvalid, "", 0
		}
	}
	host, ok := copyProxyHost(authority[:hostLen])
	if !ok {
		return ProxyRequestInvalid, "", 0
	}
	return ProxyRequestAbsolute, host, port
}

func proxyHeaderShouldStrip(name string) bool {
	switch strings.ToLower(name) {
	case "connection", "proxy-connection", "keep-alive", "transfer-encoding", "te", "trailer",
		"upgrade", "authorization", "proxy-authorization", "cookie":
		return true
	}
	return false
}

func validProxyHeaderName(name string) bool {
	if name == "" {
		return false
	}
	for index := range name {
		if name[index] <= 0x20 || name[index] == 0x7f {
			return false
		}
	}
	return true
}

func rewriteProxyHead(head string) (string, bool) {
	eol := strings.IndexByte(head, '\n')
	if eol < 0 {
		return "", false
	}
	firstSpace := strings.IndexByte(head[:eol], ' ')
	if firstSpace < 0 {
		return "", false
	}
	secondRelative := strings.IndexByte(head[firstSpace+1:eol], ' ')
	if secondRelative < 0 {
		return "", false
	}
	secondSpace := firstSpace + 1 + secondRelative
	target := head[firstSpace+1 : secondSpace]
	if !strings.HasPrefix(target, "http://") {
		return "", false
	}
	pathStart := 7
	for pathStart < len(target) && target[pathStart] != '/' {
		pathStart++
	}
	path := "/"
	if pathStart < len(target) {
		path = target[pathStart:]
	}
	version := head[secondSpace+1 : eol]
	version = strings.TrimSuffix(version, "\r")
	if len(version) < 8 {
		return "", false
	}

	var result strings.Builder
	result.Grow(len(head))
	result.WriteString(head[:firstSpace])
	result.WriteByte(' ')
	result.WriteString(path)
	result.WriteByte(' ')
	result.WriteString(version[:8])
	result.WriteString("\r\n")

	// Connection options nominate additional hop-by-hop headers. Collect every
	// header before emitting any so a late Connection header also removes an
	// earlier nominated field.
	remaining := head[eol+1:]
	headers := make([]string, 0, 16)
	connectionHeaders := make(map[string]struct{})
	for len(remaining) > 0 {
		next := strings.IndexByte(remaining, '\n')
		line := remaining
		consumed := len(remaining)
		if next >= 0 {
			line = remaining[:next+1]
			consumed = next + 1
		}
		if len(line) <= 2 {
			break
		}
		colon := strings.IndexByte(line, ':')
		if colon <= 0 || !validProxyHeaderName(line[:colon]) {
			return "", false
		}
		headers = append(headers, line)
		name := strings.ToLower(line[:colon])
		if name == "connection" || name == "proxy-connection" {
			value := strings.TrimRight(line[colon+1:], "\r\n")
			for _, option := range strings.Split(value, ",") {
				option = strings.TrimSpace(option)
				if !validProxyHeaderName(option) {
					return "", false
				}
				connectionHeaders[strings.ToLower(option)] = struct{}{}
			}
		}
		remaining = remaining[consumed:]
	}
	for _, line := range headers {
		colon := strings.IndexByte(line, ':')
		name := strings.ToLower(line[:colon])
		_, nominated := connectionHeaders[name]
		if !proxyHeaderShouldStrip(name) && !nominated {
			result.WriteString(line)
		}
	}
	result.WriteString("Connection: close\r\n\r\n")
	return result.String(), true
}

func handleProxyRequest(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded ProxyRequestPolicyRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	kind, host, port := classifyProxyRequestLine(decoded.Line)
	allowlist := DefaultPackageAllowlist
	if decoded.Allowlist != nil {
		allowlist = *decoded.Allowlist
	}
	response := ProxyRequestPolicyResponse{
		Kind: kind, Host: host, Port: port,
		Allowed: (kind == ProxyRequestConnect || kind == ProxyRequestAbsolute) &&
			proxyPortAllowed(port) && proxyHostAllowed(host, allowlist),
	}
	if response.Allowed && kind == ProxyRequestAbsolute {
		var ok bool
		response.ForwardHead, ok = rewriteProxyHead(decoded.Line)
		if !ok {
			response.Allowed = false
		}
	}
	return encode(response)
}

func handleProxyAddress(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded ProxyAddressPolicyRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	return encode(ProxyAddressPolicyResponse{Blocked: proxyIPBlocked(net.ParseIP(decoded.IP))})
}
