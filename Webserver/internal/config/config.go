// Package config loads the server configuration from a JSON file, filling in
// defaults and generating a fresh uplink token on first run.
package config

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
)

// Config is the on-disk server configuration.
type Config struct {
	// HTTPAddr is the listen address for the web UI + API (host:port).
	HTTPAddr string `json:"httpAddr"`
	// UplinkAddr is the listen address for the MainController TCP connection.
	UplinkAddr string `json:"uplinkAddr"`
	// UplinkToken is the 32-hex-char (16-byte) shared secret the MainController
	// must present in its UplinkHello frame.
	UplinkToken string `json:"uplinkToken"`
	// DataDir holds the SQLite file and uploaded assets.
	DataDir string `json:"dataDir"`
}

// Default returns the built-in defaults (no token).
func Default() Config {
	return Config{
		HTTPAddr:   ":8080",
		UplinkAddr: ":9000",
		DataDir:    "./data",
	}
}

// Load reads path, applying defaults for missing fields. If the file does not
// exist it is created with defaults and a fresh token. If it exists but has no
// token, a token is generated and written back.
func Load(path string) (Config, error) {
	cfg := Default()

	data, err := os.ReadFile(path)
	switch {
	case errors.Is(err, os.ErrNotExist):
		cfg.UplinkToken = newToken()
		if err := save(path, cfg); err != nil {
			return cfg, err
		}
		return cfg, nil
	case err != nil:
		return cfg, err
	}

	if err := json.Unmarshal(data, &cfg); err != nil {
		return cfg, err
	}
	if cfg.UplinkToken == "" {
		cfg.UplinkToken = newToken()
		if err := save(path, cfg); err != nil {
			return cfg, err
		}
	}
	return cfg, nil
}

// Token returns the 16-byte token, or an error if it is not 32 hex chars.
func (c Config) Token() ([16]byte, error) {
	var t [16]byte
	b, err := hex.DecodeString(c.UplinkToken)
	if err != nil || len(b) != 16 {
		return t, errors.New("config: uplinkToken must be 32 hex characters")
	}
	copy(t[:], b)
	return t, nil
}

// SQLitePath is the database file location.
func (c Config) SQLitePath() string { return filepath.Join(c.DataDir, "climatecontrol.db") }

// AssetDir is where uploaded floor plans and OTA images live.
func (c Config) AssetDir() string { return filepath.Join(c.DataDir, "assets") }

func newToken() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

func save(path string, cfg Config) error {
	if dir := filepath.Dir(path); dir != "" {
		_ = os.MkdirAll(dir, 0o755)
	}
	out, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(out, '\n'), 0o600)
}
