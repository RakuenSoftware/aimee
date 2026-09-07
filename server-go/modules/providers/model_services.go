package providers

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"math/big"
	"os"
	"path/filepath"
	"time"

	"github.com/JBailes/aimee/server-go/modules/providers/vaultresource"
)

// A model service belongs to its local composition, whether Server or KB. Its
// private identity persists in Vault; materialized stunnel files live in the
// deployment's private tmpfs. Nothing here chooses or starts a model.
type modelIdentity struct {
	Host       string `json:"host"`
	CA         string `json:"ca"`
	ServerCert string `json:"server_cert"`
	ServerKey  string `json:"server_key"`
	ClientCert string `json:"client_cert"`
	ClientKey  string `json:"client_key"`
}
type modelVault interface {
	Credential(context.Context, string, string, string, string) (string, error)
}

func newModelIdentity(host string, now time.Time) (modelIdentity, error) {
	result := modelIdentity{Host: host}
	serial := func() *big.Int { n, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128)); return n }
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return result, err
	}
	ca := &x509.Certificate{SerialNumber: serial(), Subject: pkix.Name{CommonName: "aimee model service CA"}, NotBefore: now.Add(-5 * time.Minute), NotAfter: now.AddDate(10, 0, 0), IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
	caDER, err := x509.CreateCertificate(rand.Reader, ca, ca, &caKey.PublicKey, caKey)
	if err != nil {
		return result, err
	}
	result.CA = string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER}))
	issue := func(client bool) (string, string, error) {
		key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			return "", "", err
		}
		cert := &x509.Certificate{SerialNumber: serial(), NotBefore: ca.NotBefore, NotAfter: now.AddDate(1, 0, 0), KeyUsage: x509.KeyUsageDigitalSignature}
		if client {
			cert.Subject.CommonName = "aimee model client"
			cert.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		} else {
			cert.Subject.CommonName = host
			cert.DNSNames = []string{host}
			cert.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
		}
		der, err := x509.CreateCertificate(rand.Reader, cert, ca, &key.PublicKey, caKey)
		if err != nil {
			return "", "", err
		}
		keyDER, err := x509.MarshalPKCS8PrivateKey(key)
		if err != nil {
			return "", "", err
		}
		defer clear(keyDER)
		return string(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})), string(pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})), nil
	}
	result.ServerCert, result.ServerKey, err = issue(false)
	if err != nil {
		return result, err
	}
	result.ClientCert, result.ClientKey, err = issue(true)
	return result, err
}

func ensureModelIdentity(ctx context.Context, store modelVault, root, profile, host string) error {
	encoded, err := store.Credential(ctx, "get", "__model_"+profile, "api_key", "")
	if err != nil {
		return err
	}
	var identity modelIdentity
	if encoded == "" {
		identity, err = newModelIdentity(host, time.Now())
		if err != nil {
			return err
		}
		data, err := json.Marshal(identity)
		if err != nil {
			return err
		}
		defer clear(data)
		if len(data) >= 4096 {
			return errors.New("model identity exceeds Vault slot capacity")
		}
		if _, err = store.Credential(ctx, "set", "__model_"+profile, "api_key", string(data)); err != nil {
			return err
		}
	} else if json.Unmarshal([]byte(encoded), &identity) != nil || identity.Host != host {
		return errors.New("invalid persisted model identity; refusing replacement")
	}
	if err = identity.validate(time.Now()); err != nil {
		return err
	}
	dir := filepath.Join(root, profile)
	if err = os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	if info, err := os.Lstat(dir); err != nil || !info.IsDir() || info.Mode().Perm() != 0700 {
		return errors.New("model identity directory must be private")
	}
	for name, data := range map[string]string{"server/ca.pem": identity.CA, "server/server.pem": identity.ServerCert, "server/server.key": identity.ServerKey, "client/ca.pem": identity.CA, "client/client.pem": identity.ClientCert, "client/client.key": identity.ClientKey} {
		leafDir := filepath.Dir(filepath.Join(dir, name))
		if err = os.MkdirAll(leafDir, 0700); err != nil {
			return err
		}
		file, err := os.CreateTemp(leafDir, ".identity-")
		if err != nil {
			return err
		}
		_, err = file.WriteString(data)
		if err == nil {
			err = file.Sync()
		}
		closeErr := file.Close()
		if err == nil {
			err = closeErr
		}
		if err == nil {
			err = os.Rename(file.Name(), filepath.Join(dir, name))
		}
		os.Remove(file.Name())
		if err != nil {
			return err
		}
	}
	return nil
}

func ModelServicesBootstrap(args []string) (bool, int) {
	if len(args) < 2 || args[1] != "__aimee_model_services" {
		return false, 0
	}
	if len(args) != 2 || filepath.Base(args[0]) != "aimee-module-providers" {
		return true, 1
	}
	home := os.Getenv("AIMEE_HOME")
	if !filepath.IsAbs(home) {
		return true, 1
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := modelServicesRootIsVolatile("/run/aimee-model-tls"); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return true, 1
	}
	unlock, err := modelServicesLock(ctx, home)
	if err != nil {
		fmt.Fprintln(os.Stderr, "model identity bootstrap lock unavailable")
		return true, 1
	}
	defer unlock()
	store := vaultresource.VaultResources{Home: home}
	for _, service := range []struct{ profile, host string }{{"embedding", "aimee-embedder"}, {"synthesis", "aimee-llm"}} {
		if err := ensureModelIdentity(ctx, store, "/run/aimee-model-tls", service.profile, service.host); err != nil {
			fmt.Fprintln(os.Stderr, "model identity:", err)
			return true, 1
		}
	}
	return true, 0
}
