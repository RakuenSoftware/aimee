package egress

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	httpResponseMagic = uint32(0x31524745) // EGR1, little endian
	httpHeaderBytes   = 12
	maxHTTPTimeout    = 2 * time.Minute
	maxHTTPHeaders    = 64 << 10
)

// HTTPRequest carries bytes to the network-owning process. RequestSHA256 is
// independently recomputed there, so a caller cannot authorize one request and
// execute another. Redirects are returned to the caller and require a new call.
type HTTPRequest struct {
	Request
	Headers            map[string]string   `json:"headers,omitempty"`
	Body               []byte              `json:"body,omitempty"`
	CredentialHandle   string              `json:"credential_handle,omitempty"`
	CredentialScope    string              `json:"credential_scope,omitempty"`
	CredentialResource string              `json:"credential_resource,omitempty"`
	Credential         *CredentialEnvelope `json:"credential,omitempty"`
	MaxResponseBytes   int64               `json:"max_response_bytes"`
	TimeoutMS          int64               `json:"timeout_ms"`
}

type HTTPResponse struct {
	Status   int
	Location string
	Body     []byte
	Error    string
}

// RequestDigest is the single request-identity grammar used by every caller
// and checked again by egress before a socket is opened.
func RequestDigest(method, target string, body []byte, credentialPresent bool) string {
	marker := byte(0)
	if credentialPresent {
		marker = 1
	}
	h := sha256.New()
	_, _ = h.Write([]byte(method))
	_, _ = h.Write([]byte{0})
	_, _ = h.Write([]byte(target))
	_, _ = h.Write([]byte{0})
	_, _ = h.Write(body)
	_, _ = h.Write([]byte{marker})
	return fmt.Sprintf("%x", h.Sum(nil))
}

func (a *BusAuthorizer) Do(ctx context.Context, traceID uint64, request HTTPRequest) (HTTPResponse, error) {
	if a == nil || a.caller == nil {
		return HTTPResponse{}, errors.New("egress: transport service is not configured")
	}
	body, err := json.Marshal(request)
	if err != nil {
		return HTTPResponse{}, err
	}
	timeout := time.Duration(request.TimeoutMS) * time.Millisecond
	if timeout <= 0 || timeout > maxHTTPTimeout {
		timeout = 30 * time.Second
	}
	reply, err := a.caller.Call(ctx, EventHTTP, StageHTTP, traceID, timeout+2*time.Second, body)
	if err != nil {
		return HTTPResponse{}, fmt.Errorf("egress transport: %w", err)
	}
	response, err := decodeHTTPResponse(reply)
	if err != nil {
		return HTTPResponse{}, err
	}
	if response.Error != "" {
		return response, errors.New(response.Error)
	}
	return response, nil
}

func encodeHTTPResponse(response HTTPResponse) ([]byte, error) {
	if len(response.Location) > 65535 || len(response.Error) > 65535 ||
		httpHeaderBytes+len(response.Location)+len(response.Error)+len(response.Body) > int(bus.ModuleMessageMaxBody) {
		return nil, errors.New("egress HTTP response exceeds the module body limit")
	}
	out := make([]byte, httpHeaderBytes+len(response.Location)+len(response.Error)+len(response.Body))
	binary.LittleEndian.PutUint32(out[0:4], httpResponseMagic)
	binary.LittleEndian.PutUint16(out[4:6], uint16(response.Status))
	binary.LittleEndian.PutUint16(out[6:8], uint16(len(response.Location)))
	binary.LittleEndian.PutUint16(out[8:10], uint16(len(response.Error)))
	copy(out[httpHeaderBytes:], response.Location)
	copy(out[httpHeaderBytes+len(response.Location):], response.Error)
	copy(out[httpHeaderBytes+len(response.Location)+len(response.Error):], response.Body)
	return out, nil
}

func decodeHTTPResponse(encoded []byte) (HTTPResponse, error) {
	if len(encoded) < httpHeaderBytes || binary.LittleEndian.Uint32(encoded[0:4]) != httpResponseMagic {
		return HTTPResponse{}, errors.New("egress transport returned an invalid response")
	}
	locationLen := int(binary.LittleEndian.Uint16(encoded[6:8]))
	errorLen := int(binary.LittleEndian.Uint16(encoded[8:10]))
	if httpHeaderBytes+locationLen+errorLen > len(encoded) {
		return HTTPResponse{}, errors.New("egress transport returned a truncated response")
	}
	offset := httpHeaderBytes
	response := HTTPResponse{Status: int(binary.LittleEndian.Uint16(encoded[4:6])),
		Location: string(encoded[offset : offset+locationLen])}
	offset += locationLen
	response.Error = string(encoded[offset : offset+errorLen])
	offset += errorLen
	response.Body = append([]byte(nil), encoded[offset:]...)
	return response, nil
}

func validHTTPHeaders(purpose string, headers map[string]string, credentialPresent bool) bool {
	for name, value := range headers {
		if strings.ContainsAny(name+value, "\r\n") {
			return false
		}
		lower := strings.ToLower(name)
		switch lower {
		case "authorization":
			return false
		case "anthropic-version":
			if purpose != "provider" {
				return false
			}
		case "accept", "content-type":
		default:
			return false
		}
	}
	return true
}

func (p policy) handleHTTP(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	var request HTTPRequest
	if json.Unmarshal(body, &request) != nil || invocation.Cancelled() ||
		request.MaxResponseBytes <= 0 || request.MaxResponseBytes > int64(bus.ModuleMessageMaxBody-httpHeaderBytes) ||
		request.TimeoutMS <= 0 || time.Duration(request.TimeoutMS)*time.Millisecond > maxHTTPTimeout ||
		!validHTTPHeaders(request.Purpose, request.Headers, request.CredentialPresent) ||
		request.RequestSHA256 != RequestDigest(request.Method, request.TargetURL, request.Body, request.CredentialPresent) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	decision := p.decide(invocation, request.Request)
	if !decision.Allowed {
		encoded, _ := encodeHTTPResponse(HTTPResponse{Error: "egress denied: " + decision.Reason})
		return encoded, bus.ModuleStatusOK
	}
	var bearer []byte
	if request.Purpose == "forge" {
		parsed, err := url.Parse(decision.Target)
		if err != nil || !forgeCredentialTargetAllowed(request.CredentialScope, request.CredentialResource,
			request.Method, parsed.EscapedPath()) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		bearer, err = p.credentials.decrypt(time.Now(), invocation, request, parsed.Hostname())
		if err != nil {
			encoded, _ := encodeHTTPResponse(HTTPResponse{Error: "egress denied: " + err.Error()})
			return encoded, bus.ModuleStatusOK
		}
	} else if request.Purpose == "provider" {
		if invocation.PrincipalRef != ProvidersClientRef {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if request.CredentialPresent {
			parsed, err := url.Parse(decision.Target)
			if err != nil {
				return nil, bus.ModuleStatusInvalidRequest
			}
			bearer, err = p.credentials.decrypt(time.Now(), invocation, request, parsed.Scheme+"://"+parsed.Host)
			if err != nil {
				encoded, _ := encodeHTTPResponse(HTTPResponse{Error: "provider credential unavailable: " + err.Error()})
				return encoded, bus.ModuleStatusOK
			}
		}
	} else if request.CredentialPresent || request.Credential != nil || request.CredentialHandle != "" ||
		request.CredentialScope != "" || request.CredentialResource != "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	response := p.executeHTTP(invocation, request, decision, bearer)
	clear(bearer)
	encoded, err := encodeHTTPResponse(response)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}

func (p policy) executeHTTP(invocation bus.ModuleInvocation, request HTTPRequest, decision Decision, bearer []byte) HTTPResponse {
	parsed, err := url.Parse(decision.Target)
	if err != nil {
		return HTTPResponse{Error: "egress: invalid authorized target"}
	}
	port := parsed.Port()
	if port == "" {
		if parsed.Scheme == "https" {
			port = "443"
		} else {
			port = "80"
		}
	}
	dialer := &net.Dialer{Timeout: time.Duration(request.TimeoutMS) * time.Millisecond}
	transport := &http.Transport{
		Proxy:                  nil,
		ResponseHeaderTimeout:  time.Duration(request.TimeoutMS) * time.Millisecond,
		MaxResponseHeaderBytes: maxHTTPHeaders,
		TLSClientConfig:        &tls.Config{MinVersion: tls.VersionTLS12, ServerName: parsed.Hostname()},
	}
	transport.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		requestedHost, _, splitErr := net.SplitHostPort(address)
		if splitErr != nil || !strings.EqualFold(requestedHost, parsed.Hostname()) || len(decision.ResolvedIPs) == 0 {
			return nil, errors.New("egress: dial target escaped the authorized host")
		}
		var last error
		for _, ip := range decision.ResolvedIPs {
			conn, dialErr := dialer.DialContext(ctx, network, net.JoinHostPort(ip, port))
			if dialErr == nil {
				return conn, nil
			}
			last = dialErr
		}
		return nil, last
	}
	client := &http.Client{Transport: transport, Timeout: time.Duration(request.TimeoutMS) * time.Millisecond,
		CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	defer transport.CloseIdleConnections()
	ctx, cancel := context.WithTimeout(context.Background(), invocation.Remaining(time.Duration(request.TimeoutMS)*time.Millisecond))
	defer cancel()
	httpRequest, err := http.NewRequestWithContext(ctx, request.Method, decision.Target, bytes.NewReader(request.Body))
	if err != nil {
		return HTTPResponse{Error: "egress: " + err.Error()}
	}
	for name, value := range request.Headers {
		httpRequest.Header.Set(name, value)
	}
	if len(bearer) > 0 {
		if request.Purpose == "provider" && request.CredentialScope == "x-api-key" {
			httpRequest.Header.Set("x-api-key", string(bearer))
		} else {
			httpRequest.Header.Set("Authorization", "Bearer "+string(bearer))
		}
	}
	response, err := client.Do(httpRequest)
	if err != nil {
		return HTTPResponse{Error: "egress: " + err.Error()}
	}
	defer response.Body.Close()
	payload, err := io.ReadAll(io.LimitReader(response.Body, request.MaxResponseBytes+1))
	if err != nil {
		return HTTPResponse{Status: response.StatusCode, Error: "egress: response read failed"}
	}
	if int64(len(payload)) > request.MaxResponseBytes {
		return HTTPResponse{Status: response.StatusCode, Error: "egress: response exceeds " + strconv.FormatInt(request.MaxResponseBytes, 10) + " bytes"}
	}
	return HTTPResponse{Status: response.StatusCode, Location: response.Header.Get("Location"), Body: payload}
}

func forgeCredentialTargetAllowed(operation, resource, method, path string) bool {
	parts := strings.Split(strings.Trim(path, "/"), "/")
	resourceParts := strings.Split(resource, "/")
	if len(parts) < 3 || len(resourceParts) != 2 || parts[0] != "repos" ||
		parts[1] != resourceParts[0] || parts[2] != resourceParts[1] {
		return false
	}
	switch operation {
	case "default_branch":
		return len(parts) == 3 && method == "GET"
	case "pr_create":
		return len(parts) == 4 && parts[3] == "pulls" && method == "POST"
	case "pr_find_open", "pr_list_open":
		return len(parts) == 4 && parts[3] == "pulls" && method == "GET"
	case "pr_info":
		return len(parts) == 5 && parts[3] == "pulls" && method == "GET"
	case "pr_edit":
		return len(parts) == 5 && parts[3] == "pulls" && method == "PATCH"
	case "pr_merge":
		return len(parts) == 6 && parts[3] == "pulls" && parts[5] == "merge" && method == "PUT"
	default:
		return false
	}
}
