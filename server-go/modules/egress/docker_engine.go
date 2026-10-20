package egress

// Docker control is a fixed Unix resource. This API exposes container-ID-bound
// observations, execution and cleanup, never an arbitrary HTTP destination.
import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
)

type DockerEngine struct {
	client    *http.Client
	transport *http.Transport
	source    *os.File
	version   string
}

type DockerExecObservation struct {
	ContainerID   string `json:"container_id"`
	ExecID        string `json:"exec_id"`
	ExitCode      string `json:"exit_code"`
	Termination   string `json:"termination"`
	Complete      bool   `json:"complete"`
	Stdout        []byte `json:"stdout_base64"`
	Stderr        []byte `json:"stderr_base64"`
	StdoutOmitted string `json:"stdout_omitted_bytes"`
	StderrOmitted string `json:"stderr_omitted_bytes"`
}

func dockerResourceID(id string) bool {
	if len(id) != 64 {
		return false
	}
	for _, c := range id {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func (engine *DockerEngine) request(ctx context.Context, method, path string, args any) (*http.Response, error) {
	var body io.Reader
	if args != nil {
		encoded, err := json.Marshal(args)
		if err != nil {
			return nil, err
		}
		body = bytes.NewReader(encoded)
	}
	request, err := http.NewRequestWithContext(ctx, method, "http://docker"+path, body)
	if err != nil {
		return nil, err
	}
	request.Header.Set("Content-Type", "application/json")
	return engine.client.Do(request)
}

func dockerResponseJSON(response *http.Response, target any) error {
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return fmt.Errorf("Docker resource status %d", response.StatusCode)
	}
	encoded, err := io.ReadAll(io.LimitReader(response.Body, (1<<20)+1))
	if err != nil || len(encoded) > 1<<20 || json.Unmarshal(encoded, target) != nil {
		return errors.New("Docker resource metadata unavailable")
	}
	return nil
}

func dockerAPIMinor(version string) (int, bool) {
	if !strings.HasPrefix(version, "1.") {
		return 0, false
	}
	minor, err := strconv.Atoi(strings.TrimPrefix(version, "1."))
	return minor, err == nil && minor >= 0 && minor <= 999 && strconv.Itoa(minor) == strings.TrimPrefix(version, "1.")
}

func (engine *DockerEngine) negotiate(ctx context.Context) error {
	response, err := engine.request(ctx, http.MethodGet, "/version", nil)
	if err != nil {
		return err
	}
	var versions struct {
		Maximum string `json:"ApiVersion"`
		Minimum string `json:"MinAPIVersion"`
	}
	if err = dockerResponseJSON(response, &versions); err != nil {
		return err
	}
	maximum, ok := dockerAPIMinor(versions.Maximum)
	if !ok || maximum < 35 {
		return errors.New("Docker API profile unavailable")
	}
	if maximum > 56 {
		maximum = 56
	}
	if versions.Minimum != "" {
		minimum, ok := dockerAPIMinor(versions.Minimum)
		if !ok || minimum > maximum {
			return errors.New("Docker API version unsupported")
		}
	}
	engine.version = "/v1." + strconv.Itoa(maximum)
	return nil
}

func (engine *DockerEngine) InspectContainer(ctx context.Context, id string) (json.RawMessage, error) {
	if !dockerResourceID(id) {
		return nil, errors.New("immutable Docker container ID required")
	}
	response, err := engine.request(ctx, http.MethodGet, engine.version+"/containers/"+id+"/json", nil)
	if err != nil {
		return nil, err
	}
	var body json.RawMessage
	if err = dockerResponseJSON(response, &body); err != nil {
		return nil, err
	}
	return body, nil
}

type dockerTail struct {
	data    []byte
	total   int64
	maximum int
}

func (tail *dockerTail) Write(data []byte) (int, error) {
	count := len(data)
	tail.total += int64(count)
	if len(data) >= tail.maximum {
		tail.data = append(tail.data[:0], data[len(data)-tail.maximum:]...)
	} else {
		if overflow := len(tail.data) + len(data) - tail.maximum; overflow > 0 {
			copy(tail.data, tail.data[overflow:])
			tail.data = tail.data[:len(tail.data)-overflow]
		}
		tail.data = append(tail.data, data...)
	}
	return count, nil
}

func readDockerOutput(reader io.Reader, stdout, stderr *dockerTail) error {
	var header [8]byte
	var total int64
	for {
		count, err := io.ReadFull(reader, header[:])
		if err == io.EOF && count == 0 {
			return nil
		}
		if err != nil || header[1] != 0 || header[2] != 0 || header[3] != 0 || (header[0] != 1 && header[0] != 2) {
			return errors.New("invalid Docker output frame")
		}
		length := int64(binary.BigEndian.Uint32(header[4:]))
		total += length
		if total > 64<<20 {
			return errors.New("Docker output budget exhausted")
		}
		writer := stdout
		if header[0] == 2 {
			writer = stderr
		}
		if _, err := io.CopyN(writer, reader, length); err != nil {
			return errors.New("incomplete Docker output frame")
		}
	}
}

func (engine *DockerEngine) Exec(ctx context.Context, container, directory, command string) (DockerExecObservation, error) {
	empty := DockerExecObservation{}
	if !dockerResourceID(container) || directory == "" || directory[0] != '/' || command == "" || len(command) > 65536 {
		return empty, errors.New("invalid owned Docker execution")
	}
	response, err := engine.request(ctx, http.MethodPost, engine.version+"/containers/"+container+"/exec", map[string]any{
		"AttachStdout": true, "AttachStderr": true, "AttachStdin": false, "Tty": false,
		"WorkingDir": directory, "Cmd": []string{"bash", "-c", command},
	})
	if err != nil {
		return empty, err
	}
	var created struct {
		ID string `json:"Id"`
	}
	if err = dockerResponseJSON(response, &created); err != nil || !dockerResourceID(created.ID) {
		return empty, errors.New("Docker exec identity unavailable")
	}
	result := DockerExecObservation{ContainerID: container, ExecID: created.ID}
	response, err = engine.request(ctx, http.MethodPost, engine.version+"/exec/"+created.ID+"/start", map[string]bool{"Detach": false, "Tty": false})
	if err != nil {
		return result, err
	}
	if response.StatusCode != http.StatusOK {
		response.Body.Close()
		return result, errors.New("Docker exec start was not acknowledged")
	}
	stdout, stderr := &dockerTail{maximum: 16384}, &dockerTail{maximum: 16384}
	err = readDockerOutput(response.Body, stdout, stderr)
	response.Body.Close()
	if err != nil {
		return result, err
	}
	response, err = engine.request(ctx, http.MethodGet, engine.version+"/exec/"+created.ID+"/json", nil)
	if err != nil {
		return result, err
	}
	var observation struct {
		ID          string `json:"ID"`
		ContainerID string `json:"ContainerID"`
		Running     *bool  `json:"Running"`
		ExitCode    *int   `json:"ExitCode"`
	}
	if err = dockerResponseJSON(response, &observation); err != nil || observation.ID != created.ID ||
		observation.ContainerID != container || observation.Running == nil || *observation.Running ||
		observation.ExitCode == nil || *observation.ExitCode < 0 || *observation.ExitCode >= 128 {
		return result, errors.New("normal Docker exec completion unverified")
	}
	result.ExitCode, result.Termination, result.Complete = strconv.Itoa(*observation.ExitCode), "exited", true
	result.Stdout, result.Stderr = stdout.data, stderr.data
	result.StdoutOmitted = strconv.FormatInt(stdout.total-int64(len(stdout.data)), 10)
	result.StderrOmitted = strconv.FormatInt(stderr.total-int64(len(stderr.data)), 10)
	return result, nil
}

func (engine *DockerEngine) Remove(ctx context.Context, container string) error {
	if !dockerResourceID(container) {
		return errors.New("immutable Docker container ID required")
	}
	response, err := engine.request(ctx, http.MethodDelete, engine.version+"/containers/"+container+"?force=1&v=1", nil)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusNoContent && response.StatusCode != http.StatusNotFound {
		return errors.New("Docker worker removal unacknowledged")
	}
	return nil
}

func (engine *DockerEngine) Close() {
	engine.transport.CloseIdleConnections()
	engine.source.Close()
}

// CreateContainer returns the immutable ID acknowledged by the held engine.
// The caller owns the container policy; this transport never follows a CLI
// environment variable to a different daemon.
func (engine *DockerEngine) CreateContainer(ctx context.Context, configuration any) (string, error) {
	response, err := engine.request(ctx, http.MethodPost, engine.version+"/containers/create", configuration)
	if err != nil {
		return "", err
	}
	var created struct {
		ID string `json:"Id"`
	}
	if err := dockerResponseJSON(response, &created); err != nil {
		return "", err
	}
	if !dockerResourceID(created.ID) {
		return "", errors.New("Docker creation identity unavailable")
	}
	return created.ID, nil
}

func (engine *DockerEngine) StartContainer(ctx context.Context, id string) error {
	if !dockerResourceID(id) {
		return errors.New("immutable Docker container ID required")
	}
	response, err := engine.request(ctx, http.MethodPost, engine.version+"/containers/"+id+"/start", nil)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusNoContent {
		return errors.New("fresh Docker worker start unacknowledged")
	}
	return nil
}

// ListLabeledContainers selects only the caller's exact managed label scope.
// Callers must re-inspect each immutable ID before removing it.
func (engine *DockerEngine) ListLabeledContainers(ctx context.Context, labels map[string]string) ([]string, error) {
	filters := make([]string, 0, len(labels))
	for key, value := range labels {
		filters = append(filters, key+"="+value)
	}
	encoded, err := json.Marshal(map[string][]string{"label": filters})
	if err != nil || len(labels) == 0 {
		return nil, errors.New("managed label scope required")
	}
	response, err := engine.request(ctx, http.MethodGet, engine.version+"/containers/json?all=1&filters="+url.QueryEscape(string(encoded)), nil)
	if err != nil {
		return nil, err
	}
	var rows []struct {
		ID string `json:"Id"`
	}
	if err := dockerResponseJSON(response, &rows); err != nil {
		return nil, err
	}
	if len(rows) > 4096 {
		return nil, errors.New("managed container list exceeds bound")
	}
	ids := make([]string, 0, len(rows))
	for _, row := range rows {
		if !dockerResourceID(row.ID) {
			return nil, errors.New("managed container identity unavailable")
		}
		ids = append(ids, row.ID)
	}
	return ids, nil
}
