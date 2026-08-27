package egress

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	credentialVersion      = 1
	credentialHandleForge  = "forge"
	credentialHostForge    = "api.github.com"
	credentialMaxLifetime  = 60 * time.Second
	credentialKeyIDBytes   = 16
	credentialPublicBytes  = 32
	credentialNonceBytes   = 12
	credentialTagBytes     = 16
	credentialMaxPlaintext = 4096
)

var (
	credentialKDFDomain = []byte("aimee.egress.x25519.v1\x00")
	credentialAADDomain = []byte("aimee.egress.credential.v1\x00")
)

// CredentialEnvelope is safe to relay through a caller process: it contains a
// short-lived ciphertext bound to one egress identity, caller, forge operation,
// host and repository. It never contains a reusable bearer in plaintext.
type CredentialEnvelope struct {
	Version            int    `json:"version"`
	KeyID              string `json:"key_id"`
	EphemeralPublicKey string `json:"ephemeral_public_key"`
	Nonce              string `json:"nonce"`
	Ciphertext         string `json:"ciphertext"`
	ExpiresAt          int64  `json:"expires_at"`
	Handle             string `json:"handle"`
	Host               string `json:"host"`
	Operation          string `json:"operation"`
	Resource           string `json:"resource"`
	PrincipalRef       uint32 `json:"principal_ref"`
}

type credentialBroker struct {
	private *ecdh.PrivateKey
	keyID   string
}

func newCredentialBroker() (*credentialBroker, error) {
	private, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return nil, fmt.Errorf("egress credential key generation: %w", err)
	}
	id := make([]byte, credentialKeyIDBytes)
	if _, err := rand.Read(id); err != nil {
		return nil, fmt.Errorf("egress credential key id generation: %w", err)
	}
	return &credentialBroker{private: private, keyID: hex.EncodeToString(id)}, nil
}

func (b *credentialBroker) publicReply() ([]byte, bus.ModuleStatus) {
	if b == nil || b.private == nil || len(b.keyID) != credentialKeyIDBytes*2 {
		return nil, bus.ModuleStatusInternal
	}
	reply, err := json.Marshal(struct {
		Version   int    `json:"version"`
		KeyID     string `json:"key_id"`
		PublicKey string `json:"public_key"`
	}{credentialVersion, b.keyID, base64.StdEncoding.EncodeToString(b.private.PublicKey().Bytes())})
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

func credentialAAD(envelope CredentialEnvelope) ([]byte, error) {
	if envelope.Version != credentialVersion || envelope.Handle != credentialHandleForge ||
		envelope.Host != credentialHostForge || envelope.PrincipalRef != GitClientRef ||
		!forgeOperation(envelope.Operation) || !forgeResource(envelope.Resource) || envelope.ExpiresAt <= 0 {
		return nil, errors.New("invalid credential scope")
	}
	fields := []string{envelope.Handle, envelope.Host, envelope.Operation, envelope.Resource}
	length := len(credentialAADDomain) + 4 + 8
	for _, field := range fields {
		if strings.IndexByte(field, 0) >= 0 {
			return nil, errors.New("invalid credential scope")
		}
		length += len(field) + 1
	}
	aad := make([]byte, 0, length)
	aad = append(aad, credentialAADDomain...)
	for _, field := range fields {
		aad = append(aad, field...)
		aad = append(aad, 0)
	}
	var numeric [12]byte
	binary.BigEndian.PutUint32(numeric[:4], envelope.PrincipalRef)
	binary.BigEndian.PutUint64(numeric[4:], uint64(envelope.ExpiresAt))
	aad = append(aad, numeric[:]...)
	return aad, nil
}

func (b *credentialBroker) decrypt(now time.Time, invocation bus.ModuleInvocation,
	request HTTPRequest, targetHost string) ([]byte, error) {
	if b == nil || b.private == nil || request.Credential == nil ||
		request.CredentialHandle != credentialHandleForge || request.CredentialScope == "" ||
		request.CredentialResource == "" {
		return nil, errors.New("credential handle is unavailable")
	}
	envelope := *request.Credential
	if invocation.PrincipalRef != envelope.PrincipalRef || request.CredentialHandle != envelope.Handle ||
		request.CredentialScope != envelope.Operation || request.CredentialResource != envelope.Resource ||
		!strings.EqualFold(targetHost, envelope.Host) || envelope.KeyID != b.keyID {
		return nil, errors.New("credential scope mismatch")
	}
	expires := time.Unix(envelope.ExpiresAt, 0)
	if !now.Before(expires) || expires.Sub(now) > credentialMaxLifetime {
		return nil, errors.New("credential envelope is expired")
	}
	peerBytes, err := base64.StdEncoding.Strict().DecodeString(envelope.EphemeralPublicKey)
	if err != nil || len(peerBytes) != credentialPublicBytes {
		return nil, errors.New("invalid credential public key")
	}
	nonce, err := base64.StdEncoding.Strict().DecodeString(envelope.Nonce)
	if err != nil || len(nonce) != credentialNonceBytes {
		return nil, errors.New("invalid credential nonce")
	}
	ciphertext, err := base64.StdEncoding.Strict().DecodeString(envelope.Ciphertext)
	if err != nil || len(ciphertext) <= credentialTagBytes || len(ciphertext) > credentialMaxPlaintext+credentialTagBytes {
		return nil, errors.New("invalid credential ciphertext")
	}
	peer, err := ecdh.X25519().NewPublicKey(peerBytes)
	if err != nil {
		return nil, errors.New("invalid credential public key")
	}
	shared, err := b.private.ECDH(peer)
	if err != nil {
		return nil, errors.New("credential key agreement failed")
	}
	keyInput := make([]byte, 0, len(credentialKDFDomain)+len(shared)+credentialPublicBytes*2)
	keyInput = append(keyInput, credentialKDFDomain...)
	keyInput = append(keyInput, shared...)
	keyInput = append(keyInput, peerBytes...)
	keyInput = append(keyInput, b.private.PublicKey().Bytes()...)
	key := sha256.Sum256(keyInput)
	clear(shared)
	clear(keyInput)
	block, err := aes.NewCipher(key[:])
	clear(key[:])
	if err != nil {
		return nil, errors.New("credential cipher setup failed")
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		return nil, errors.New("credential cipher setup failed")
	}
	aad, err := credentialAAD(envelope)
	if err != nil {
		return nil, err
	}
	plaintext, err := gcm.Open(nil, nonce, ciphertext, aad)
	if err != nil || len(plaintext) == 0 || len(plaintext) > credentialMaxPlaintext {
		clear(plaintext)
		return nil, errors.New("credential authentication failed")
	}
	return plaintext, nil
}

func forgeOperation(operation string) bool {
	switch operation {
	case "default_branch", "pr_create", "pr_find_open", "pr_list_open", "pr_info", "pr_edit", "pr_merge":
		return true
	default:
		return false
	}
}

func forgeResource(resource string) bool {
	parts := strings.Split(resource, "/")
	return len(parts) == 2 && validForgeName(parts[0]) && validForgeName(parts[1])
}

func validForgeName(name string) bool {
	if name == "" || len(name) > 100 {
		return false
	}
	for index := range len(name) {
		char := name[index]
		if !(char >= 'a' && char <= 'z' || char >= 'A' && char <= 'Z' || char >= '0' && char <= '9' ||
			char == '-' || char == '_' || char == '.') {
			return false
		}
	}
	return true
}
