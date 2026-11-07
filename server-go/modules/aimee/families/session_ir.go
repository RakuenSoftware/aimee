package families

import (
	"bytes"
	"encoding/json"

	canonical "github.com/JBailes/aimee/server-go/ir"
)

func validSessionIREvent(encoded []byte) bool { return canonical.ValidSessionEvent(encoded) }
func decodeIR(encoded []byte, out any) bool   { return canonical.Decode(encoded, out) }

// Avoid HTML-escaping a 1 MiB payload into a >3 MiB envelope. The browser receives
// JSON as data, never HTML. Page sizing and final serialization must use the same
// encoder, otherwise a bounded page can expand beyond its declared reply budget.
func encodeSessionIR(value any) ([]byte, error) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	if err := encoder.Encode(value); err != nil {
		return nil, err
	}
	return bytes.TrimSuffix(buffer.Bytes(), []byte("\n")), nil
}
