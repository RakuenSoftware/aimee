package providers

import (
	"crypto/tls"
	"crypto/x509"
	"errors"
	"time"
)

func (i modelIdentity) validate(now time.Time) error {
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM([]byte(i.CA)) {
		return errors.New("invalid model CA")
	}
	for _, leaf := range []struct {
		cert, key string
		client    bool
	}{{i.ServerCert, i.ServerKey, false}, {i.ClientCert, i.ClientKey, true}} {
		pair, err := tls.X509KeyPair([]byte(leaf.cert), []byte(leaf.key))
		if err != nil {
			return errors.New("invalid model certificate/key")
		}
		cert, err := x509.ParseCertificate(pair.Certificate[0])
		if err != nil {
			return err
		}
		opts := x509.VerifyOptions{Roots: roots, CurrentTime: now, KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}, DNSName: i.Host}
		if leaf.client {
			opts.DNSName = ""
			opts.KeyUsages = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		}
		if _, err = cert.Verify(opts); err != nil {
			return errors.New("model certificate verification failed")
		}
	}
	return nil
}
