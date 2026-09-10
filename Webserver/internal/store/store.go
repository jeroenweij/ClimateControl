// Package store is the SQLite persistence layer: node roster, the readings
// time series, operator commands, config overrides, OTA jobs, and the map
// tables. See MainController-Server-Link-Spec.md §6.
package store

import (
	"context"
	"database/sql"
	_ "embed"
	"fmt"
	"strings"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	_ "modernc.org/sqlite"
)

//go:embed schema.sql
var schema string

// Store wraps the database handle.
type Store struct {
	db *sql.DB
}

// Open connects to (creating if needed) the SQLite file at path and applies
// the schema. A path of ":memory:" gives an ephemeral store for tests.
func Open(path string) (*Store, error) {
	dsn := path + "?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=foreign_keys(1)"
	if path == ":memory:" {
		dsn = "file::memory:?cache=shared"
	}
	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1) // one writer; SQLite serialises anyway
	if _, err := db.Exec(schema); err != nil {
		db.Close()
		return nil, fmt.Errorf("apply schema: %w", err)
	}
	if err := migrate(db); err != nil {
		db.Close()
		return nil, fmt.Errorf("migrate: %w", err)
	}
	return &Store{db: db}, nil
}

// migrate applies additive schema changes that CREATE TABLE IF NOT EXISTS
// cannot make to a database created by an older build. Each step is
// idempotent: a "duplicate column" error means it is already applied.
func migrate(db *sql.DB) error {
	steps := []string{
		`ALTER TABLE ota_jobs ADD COLUMN target TEXT NOT NULL DEFAULT 'node'`,
	}
	for _, s := range steps {
		if _, err := db.Exec(s); err != nil && !strings.Contains(err.Error(), "duplicate column name") {
			return err
		}
	}
	return nil
}

// Close releases the database.
func (s *Store) Close() error { return s.db.Close() }

// Node is a roster row.
type Node struct {
	ID        int    `json:"id"`
	Module    string `json:"module"`
	FWVersion int    `json:"fwVersion"`
	FirstSeen int64  `json:"firstSeen"`
	LastSeen  int64  `json:"lastSeen"`
	State     int    `json:"state"`
	Online    bool   `json:"online"`
}

// UpsertNode records a sighting, updating module/last_seen/online.
func (s *Store) UpsertNode(ctx context.Context, id int, module nodelib.Module, online bool) error {
	now := time.Now().Unix()
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO nodes (id, module, first_seen, last_seen, online)
		VALUES (?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
			module    = CASE WHEN excluded.module != 0 THEN excluded.module ELSE nodes.module END,
			last_seen = excluded.last_seen,
			online    = excluded.online`,
		id, int(module), now, now, boolInt(online))
	return err
}

// SetNodeFirmware records a node's running firmware version (major<<8 | minor),
// as learned from a SystemInfo report. A zero version is ignored.
func (s *Store) SetNodeFirmware(ctx context.Context, id, version int) error {
	if version == 0 {
		return nil
	}
	_, err := s.db.ExecContext(ctx,
		`UPDATE nodes SET fw_version = ? WHERE id = ?`, version, id)
	return err
}

// SetNodeOnline flips just the online flag (heartbeat / presence).
func (s *Store) SetNodeOnline(ctx context.Context, id int, online bool) error {
	_, err := s.db.ExecContext(ctx,
		`UPDATE nodes SET online = ?, last_seen = ? WHERE id = ?`,
		boolInt(online), time.Now().Unix(), id)
	return err
}

// Nodes returns the full roster, ascending by id.
func (s *Store) Nodes(ctx context.Context) ([]Node, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, module, fw_version, first_seen, last_seen, state, online FROM nodes ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Node{}
	for rows.Next() {
		var n Node
		var mod, online int
		if err := rows.Scan(&n.ID, &mod, &n.FWVersion, &n.FirstSeen, &n.LastSeen, &n.State, &online); err != nil {
			return nil, err
		}
		n.Module = nodelib.Module(mod).String()
		n.Online = online != 0
		out = append(out, n)
	}
	return out, rows.Err()
}

// InsertReading stores one decoded Report.
func (s *Store) InsertReading(ctx context.Context, tsMillis int64, nodeID int, ep nodelib.Endpoint, raw []byte, v nodelib.Value) error {
	var num sql.NullFloat64
	var text sql.NullString
	switch v.Kind {
	case "number", "enum":
		num = sql.NullFloat64{Float64: v.Num, Valid: true}
		if v.Text != "" {
			text = sql.NullString{String: v.Text, Valid: true}
		}
	default:
		if v.Text != "" {
			text = sql.NullString{String: v.Text, Valid: true}
		}
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO readings (ts, node_id, endpoint, raw, value_num, value_text) VALUES (?, ?, ?, ?, ?, ?)`,
		tsMillis, nodeID, int(ep), raw, num, text)
	return err
}

// ReadingPoint is one historical sample.
type ReadingPoint struct {
	TS   int64    `json:"ts"`
	Num  *float64 `json:"num,omitempty"`
	Text string   `json:"text,omitempty"`
}

// Readings returns the series for one node/endpoint within [fromMs, toMs],
// newest first, capped at limit.
func (s *Store) Readings(ctx context.Context, nodeID int, ep nodelib.Endpoint, fromMs, toMs int64, limit int) ([]ReadingPoint, error) {
	if limit <= 0 || limit > 50000 {
		limit = 5000
	}
	rows, err := s.db.QueryContext(ctx, `
		SELECT ts, value_num, value_text FROM readings
		WHERE node_id = ? AND endpoint = ? AND ts BETWEEN ? AND ?
		ORDER BY ts DESC LIMIT ?`,
		nodeID, int(ep), fromMs, toMs, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []ReadingPoint{}
	for rows.Next() {
		var p ReadingPoint
		var num sql.NullFloat64
		var text sql.NullString
		if err := rows.Scan(&p.TS, &num, &text); err != nil {
			return nil, err
		}
		if num.Valid {
			p.Num = &num.Float64
		}
		p.Text = text.String
		out = append(out, p)
	}
	return out, rows.Err()
}

// LogCommand records an operator downlink for the audit trail.
func (s *Store) LogCommand(ctx context.Context, nodeID int, ep nodelib.Endpoint, op nodelib.Operation, payload []byte, user string) (int64, error) {
	res, err := s.db.ExecContext(ctx,
		`INSERT INTO commands (ts, node_id, endpoint, operation, payload, "user", sent_ts)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		time.Now().UnixMilli(), nodeID, int(ep), int(op), payload, user, time.Now().UnixMilli())
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// Override is a desired value the server re-asserts on node rejoin.
type Override struct {
	NodeID   int     `json:"nodeId"`
	Endpoint string  `json:"endpoint"`
	Value    float64 `json:"value"`
	User     string  `json:"user"`
	TS       int64   `json:"ts"`
}

// SetOverride upserts one desired value.
func (s *Store) SetOverride(ctx context.Context, nodeID int, ep nodelib.Endpoint, value float64, user string) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO config_overrides (node_id, endpoint, value, "user", ts)
		VALUES (?, ?, ?, ?, ?)
		ON CONFLICT(node_id, endpoint) DO UPDATE SET value = excluded.value, "user" = excluded."user", ts = excluded.ts`,
		nodeID, int(ep), value, user, time.Now().UnixMilli())
	return err
}

// DeleteOverride drops one desired value.
func (s *Store) DeleteOverride(ctx context.Context, nodeID int, ep nodelib.Endpoint) error {
	_, err := s.db.ExecContext(ctx,
		`DELETE FROM config_overrides WHERE node_id = ? AND endpoint = ?`, nodeID, int(ep))
	return err
}

// Overrides lists all desired values, optionally filtered to one node (nodeID
// <= 0 for all).
func (s *Store) Overrides(ctx context.Context, nodeID int) ([]Override, error) {
	q := `SELECT node_id, endpoint, value, "user", ts FROM config_overrides`
	var args []any
	if nodeID > 0 {
		q += ` WHERE node_id = ?`
		args = append(args, nodeID)
	}
	q += ` ORDER BY node_id, endpoint`
	rows, err := s.db.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []Override{}
	for rows.Next() {
		var o Override
		var ep int
		if err := rows.Scan(&o.NodeID, &ep, &o.Value, &o.User, &o.TS); err != nil {
			return nil, err
		}
		o.Endpoint = nodelib.Endpoint(ep).String()
		out = append(out, o)
	}
	return out, rows.Err()
}

// DB exposes the handle for the map + OTA stores in this package.
func (s *Store) DB() *sql.DB { return s.db }

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}
