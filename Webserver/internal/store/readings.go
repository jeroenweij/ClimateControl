package store

import (
	"bytes"
	"context"
	"database/sql"

	"climatecontrol/webserver/internal/nodelib"
)

// RepeatInterval is how often an unchanged value is stored anyway. A node
// re-reports every published value about once a minute as a keepalive; those
// repeats carry no new information, so only a change -- or the first repeat
// once this long has passed since the series' last row -- becomes a row.
// Shorter than an hour, so a node that keeps reporting leaves at least one row
// in every hour (the rollup in retention.go relies on that to tell a held value
// from a silent node).
const RepeatInterval = 30 * 60 * 1000 // ms

type seriesKey struct {
	node int
	ep   nodelib.Endpoint
}

type storedMark struct {
	ts  int64
	raw []byte
}

// InsertReading stores one decoded Report, unless it repeats the series' last
// stored value within RepeatInterval. The history is step-shaped: each row's
// value holds until the next row.
func (s *Store) InsertReading(ctx context.Context, tsMillis int64, nodeID int, ep nodelib.Endpoint, raw []byte, v nodelib.Value) error {
	key := seriesKey{nodeID, ep}
	s.mu.Lock()
	last, seen := s.lastStored[key]
	s.mu.Unlock()
	if seen && bytes.Equal(last.raw, raw) && tsMillis-last.ts < RepeatInterval {
		return nil
	}

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
	if err != nil {
		return err
	}
	s.mu.Lock()
	s.lastStored[key] = storedMark{ts: tsMillis, raw: bytes.Clone(raw)}
	s.mu.Unlock()
	return nil
}

// ReadingPoint is one historical sample. A point from readings_hourly (older
// than the raw retention window) has TS at the start of its hour, Num its
// time-weighted mean, and Min/Max/N set; a raw point has neither.
type ReadingPoint struct {
	TS   int64    `json:"ts"`
	Num  *float64 `json:"num,omitempty"`
	Text string   `json:"text,omitempty"`
	Min  *float64 `json:"min,omitempty"`
	Max  *float64 `json:"max,omitempty"`
	N    int      `json:"n,omitempty"`
}

// Readings returns the series for one node/endpoint within [fromMs, toMs],
// newest first, capped at limit: the raw rows, then -- further back, where
// only the rollup is kept -- the hourly points.
func (s *Store) Readings(ctx context.Context, nodeID int, ep nodelib.Endpoint, fromMs, toMs int64, limit int) ([]ReadingPoint, error) {
	if limit <= 0 || limit > 50000 {
		limit = 5000
	}
	out, err := s.rawReadings(ctx, nodeID, ep, fromMs, toMs, limit)
	if err != nil || len(out) >= limit {
		return out, err
	}
	hourly, err := s.hourlyReadings(ctx, nodeID, ep, fromMs, toMs, limit-len(out))
	return append(out, hourly...), err
}

func (s *Store) rawReadings(ctx context.Context, nodeID int, ep nodelib.Endpoint, fromMs, toMs int64, limit int) ([]ReadingPoint, error) {
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

func (s *Store) hourlyReadings(ctx context.Context, nodeID int, ep nodelib.Endpoint, fromMs, toMs int64, limit int) ([]ReadingPoint, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT hour, avg, min, max, n FROM readings_hourly
		WHERE node_id = ? AND endpoint = ? AND hour BETWEEN ? AND ?
		ORDER BY hour DESC LIMIT ?`,
		nodeID, int(ep), fromMs, toMs, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []ReadingPoint{}
	for rows.Next() {
		var p ReadingPoint
		var avg, lo, hi float64
		if err := rows.Scan(&p.TS, &avg, &lo, &hi, &p.N); err != nil {
			return nil, err
		}
		p.Num, p.Min, p.Max = &avg, &lo, &hi
		out = append(out, p)
	}
	return out, rows.Err()
}
