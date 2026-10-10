package memory

// Failure is a stable, payload-free refusal carried by the additive external
// data contract. It must never be interpreted as an empty successful search.
type Failure struct {
	Kind      string `json:"kind"`
	Message   string `json:"message"`
	Retryable bool   `json:"retryable"`
}

func (f *Failure) Error() string { return "memory: " + f.Kind }
func (f *Failure) Unwrap() error {
	switch f.Kind {
	case "capacity":
		return ErrCapacity
	case "unsupported":
		return ErrUnsupported
	case "invalid_request":
		return ErrClientRequest
	case "not_found":
		return ErrNotFound
	default:
		return ErrUnavailable
	}
}
