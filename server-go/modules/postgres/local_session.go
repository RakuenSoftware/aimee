package postgres

// Local sessions support offline bootstrap and narrowly privileged operator
// tools before a module bus exists. Credentials arrive on an inherited private
// pipe, never argv, logs, the event bus, or an externally listening socket.
import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/jackc/pgx/v5/pgxpool"
)

// ServeLocalSession uses the same session engine as the supervised provider.
// The caller owns this process and its sole session. Closing the channel closes
// the connection, rolling back unfinished work and releasing advisory locks.
func ServeLocalSession(ctx context.Context, input io.Reader, output io.Writer) error {
	credential, err := readLocalFrame(input, 4099)
	if err != nil {
		return errors.New("postgres: local credential frame unavailable")
	}
	if len(credential) < 4 {
		clear(credential)
		return errors.New("postgres: invalid local authority")
	}
	policy := binary.LittleEndian.Uint32(credential[:4])
	dsn := string(credential[4:])
	if (policy == 0 || policy == 1) && dsn == "" {
		clear(credential)
		return errors.New("postgres: explicit authority is empty")
	}
	var config *pgxpool.Config
	var pool *pgxpool.Pool
	switch policy {
	case 0:
		config, err = parseStoreConfigFor("local authority", dsn)
	case 1:
		config, err = operatorConfig(dsn)
	case 2, 3:
		// The fixed deployment credential is resolved by its owning provider. The
		// native bootstrap caller neither receives it nor supplies an alternate DSN.
		if dsn != "" {
			clear(credential)
			return errors.New("postgres: configured authority does not accept credentials")
		}
		if policy == 2 {
			pool, err = MigrationPool(ctx)
		} else {
			pool, err = SQLPool(ctx)
		}
		if err == nil {
			config = pool.Config()
		}
	default:
		clear(credential)
		return errors.New("postgres: invalid local authority policy")
	}
	clear(credential)
	if err != nil {
		return err
	}
	if policy == 0 {
		if err := validateRuntimeTLS(config); err != nil {
			return err
		}
	}
	if pool == nil {
		config.MaxConns = 1
		config.MinConns = 0
		if policy == 0 {
			config.ConnConfig.ConnectTimeout = 10 * time.Second
		}
		pool, err = pgxpool.NewWithConfig(ctx, config)
		if err != nil {
			return errors.New("postgres: local authority pool unavailable")
		}
	}
	defer pool.Close()
	lifetime, cancel := context.WithCancel(ctx)
	defer cancel()
	h := &sessionHandler{sessions: make(map[uint64]*postgresSession), poolFn: func(context.Context) (*pgxpool.Pool, error) { return pool, nil }, idle: 90 * time.Second, ctx: lifetime}
	defer h.close()
	for {
		body, err := readLocalFrame(input, int(bus.ModuleMessageMaxBody)+8)
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return errors.New("postgres: invalid local request frame")
		}
		if len(body) < 8 {
			return errors.New("postgres: local request deadline missing")
		}
		deadline := binary.LittleEndian.Uint64(body[:8])
		reply, status := h.handle(bus.ModuleInvocation{StageID: StageSession, DeadlineNS: deadline}, body[8:])
		if status != bus.ModuleStatusOK {
			reply = refuse(statusFailed, "08006", "PostgreSQL local request refused")
		}
		if err := writeLocalFrame(output, reply); err != nil {
			return errors.New("postgres: local reply channel closed")
		}
	}
}
func readLocalFrame(input io.Reader, maximum int) ([]byte, error) {
	var length [4]byte
	if _, err := io.ReadFull(input, length[:]); err != nil {
		return nil, err
	}
	n := binary.LittleEndian.Uint32(length[:])
	if n == 0 || uint64(n) > uint64(maximum) {
		return nil, errors.New("invalid local frame length")
	}
	body := make([]byte, n)
	if _, err := io.ReadFull(input, body); err != nil {
		clear(body)
		return nil, err
	}
	return body, nil
}
func writeLocalFrame(output io.Writer, body []byte) error {
	var length [4]byte
	binary.LittleEndian.PutUint32(length[:], uint32(len(body)))
	for _, part := range [][]byte{length[:], body} {
		for len(part) > 0 {
			n, err := output.Write(part)
			if err != nil {
				return err
			}
			if n <= 0 {
				return io.ErrShortWrite
			}
			part = part[n:]
		}
	}
	return nil
}
