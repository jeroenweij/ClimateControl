package store

import (
	"context"
	"database/sql"
	"log/slog"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

const (
	hourMs = int64(time.Hour / time.Millisecond)

	// How long a stored value is taken to hold when no newer row follows it.
	// A node that keeps reporting leaves a row at least every RepeatInterval,
	// so a longer silence means the node was gone and the hour isn't covered.
	holdMs = hourMs

	// Raw rows are rolled up and deleted one day of one series per
	// transaction, so the single connection is never held for long against
	// the uplink's inserts.
	rollupChunkMs = 24 * hourMs
)

// RollupStats summarises one RollUp pass.
type RollupStats struct {
	Series  int   // series that had rows rolled up
	Hours   int   // readings_hourly rows written
	Deleted int64 // raw rows deleted
}

// RunRetention rolls up raw readings older than keep (RollUp) once right away
// and then every hour, until ctx is done.
func (s *Store) RunRetention(ctx context.Context, keep time.Duration, log *slog.Logger) {
	pass := func() {
		stats, err := s.RollUp(ctx, time.Now(), keep)
		if err != nil && ctx.Err() == nil {
			log.Warn("readings rollup", "err", err)
		}
		if stats.Deleted > 0 {
			log.Info("readings rolled up", "series", stats.Series, "hours", stats.Hours, "deleted", stats.Deleted)
		}
	}
	pass()
	tick := time.NewTicker(time.Hour)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			pass()
		}
	}
}

// RollUp folds every raw reading from before the hour boundary at or below
// now-keep into readings_hourly and deletes it. Numeric series get one row per
// hour they were known in; text/struct series (value_num NULL) are only
// deleted.
func (s *Store) RollUp(ctx context.Context, now time.Time, keep time.Duration) (RollupStats, error) {
	cutoff := floorHour(now.Add(-keep).UnixMilli())
	var stats RollupStats
	node, ep := -1, -1
	for {
		err := s.db.QueryRowContext(ctx, `
			SELECT node_id, endpoint FROM readings
			WHERE (node_id, endpoint) > (?, ?)
			ORDER BY node_id, endpoint LIMIT 1`, node, ep).Scan(&node, &ep)
		if err == sql.ErrNoRows {
			return stats, nil
		}
		if err != nil {
			return stats, err
		}
		hours, deleted, err := s.rollUpSeries(ctx, seriesKey{node, nodelib.Endpoint(ep)}, cutoff)
		if hours > 0 || deleted > 0 {
			stats.Series++
		}
		stats.Hours += hours
		stats.Deleted += deleted
		if err != nil {
			return stats, err
		}
	}
}

type sample struct {
	ts  int64
	num float64
}

func (s *Store) rollUpSeries(ctx context.Context, key seriesKey, cutoff int64) (hours int, deleted int64, err error) {
	var minTS sql.NullInt64
	err = s.db.QueryRowContext(ctx,
		`SELECT MIN(ts) FROM readings WHERE node_id = ? AND endpoint = ?`,
		key.node, int(key.ep)).Scan(&minTS)
	if err != nil || !minTS.Valid || minTS.Int64 >= cutoff {
		return 0, 0, err
	}
	first := floorHour(minTS.Int64)

	// Continue where the previous pass stopped, carrying in the value that
	// was still holding there -- unless it ran out before the next raw row.
	from := first
	var carry *sample
	var rolledTo, lastTS sql.NullInt64
	var lastNum sql.NullFloat64
	err = s.db.QueryRowContext(ctx,
		`SELECT rolled_to, last_ts, last_num FROM readings_rollup WHERE node_id = ? AND endpoint = ?`,
		key.node, int(key.ep)).Scan(&rolledTo, &lastTS, &lastNum)
	switch {
	case err == sql.ErrNoRows:
	case err != nil:
		return 0, 0, err
	default:
		// Never below rolled_to: those hours are final. A raw row still
		// sitting there (the server clock stepped back) is only deleted.
		from = max(first, rolledTo.Int64)
		if lastTS.Valid && lastNum.Valid && lastTS.Int64+holdMs > rolledTo.Int64 {
			carry = &sample{lastTS.Int64, lastNum.Float64}
			from = rolledTo.Int64
		}
	}

	for start := from; start < cutoff; {
		end := min(start+rollupChunkMs, cutoff)
		samples, err := s.numericSamples(ctx, key, start, end)
		if err != nil {
			return hours, deleted, err
		}
		aggs := aggregateHours(carry, samples, start, end)
		if len(samples) > 0 {
			carry = &samples[len(samples)-1]
		}
		n, err := s.commitChunk(ctx, key, aggs, end, carry)
		hours += len(aggs)
		deleted += n
		if err != nil {
			return hours, deleted, err
		}
		start = end
	}
	return hours, deleted, nil
}

func (s *Store) numericSamples(ctx context.Context, key seriesKey, start, end int64) ([]sample, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT ts, value_num FROM readings
		WHERE node_id = ? AND endpoint = ? AND ts >= ? AND ts < ? AND value_num IS NOT NULL
		ORDER BY ts`,
		key.node, int(key.ep), start, end)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []sample
	for rows.Next() {
		var p sample
		if err := rows.Scan(&p.ts, &p.num); err != nil {
			return nil, err
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// commitChunk writes one chunk's hourly rows, deletes every raw row of the
// series before end and records the progress -- atomically, so an
// interrupted pass neither loses nor double-counts an hour.
func (s *Store) commitChunk(ctx context.Context, key seriesKey, aggs []hourAgg, end int64, carry *sample) (int64, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return 0, err
	}
	defer tx.Rollback()
	for _, a := range aggs {
		if _, err := tx.ExecContext(ctx, `
			INSERT OR REPLACE INTO readings_hourly (node_id, endpoint, hour, min, avg, max, n)
			VALUES (?, ?, ?, ?, ?, ?, ?)`,
			key.node, int(key.ep), a.hour, a.min, a.avg(), a.max, a.n); err != nil {
			return 0, err
		}
	}
	res, err := tx.ExecContext(ctx,
		`DELETE FROM readings WHERE node_id = ? AND endpoint = ? AND ts < ?`,
		key.node, int(key.ep), end)
	if err != nil {
		return 0, err
	}
	deleted, _ := res.RowsAffected()
	var lastTS sql.NullInt64
	var lastNum sql.NullFloat64
	if carry != nil {
		lastTS = sql.NullInt64{Int64: carry.ts, Valid: true}
		lastNum = sql.NullFloat64{Float64: carry.num, Valid: true}
	}
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO readings_rollup (node_id, endpoint, rolled_to, last_ts, last_num)
		VALUES (?, ?, ?, ?, ?)
		ON CONFLICT(node_id, endpoint) DO UPDATE SET
			rolled_to = excluded.rolled_to, last_ts = excluded.last_ts, last_num = excluded.last_num`,
		key.node, int(key.ep), end, lastTS, lastNum); err != nil {
		return 0, err
	}
	return deleted, tx.Commit()
}

type hourAgg struct {
	hour     int64
	min, max float64
	wsum     float64 // value x ms held
	dur      int64   // ms the value was known
	psum     float64 // sum of the raw values inside the hour
	n        int     // raw rows inside the hour
	seen     bool
}

func (a *hourAgg) extend(v float64) {
	if !a.seen || v < a.min {
		a.min = v
	}
	if !a.seen || v > a.max {
		a.max = v
	}
	a.seen = true
}

// avg is time-weighted; an hour whose only rows were superseded at once
// (zero hold) falls back to their plain mean.
func (a *hourAgg) avg() float64 {
	if a.dur > 0 {
		return a.wsum / float64(a.dur)
	}
	return a.psum / float64(a.n)
}

// aggregateHours buckets a step-shaped series into the hours of [start, end):
// each sample holds until the next one, or for holdMs at most. carry is the
// last sample before start, if any.
func aggregateHours(carry *sample, samples []sample, start, end int64) []hourAgg {
	buckets := make([]hourAgg, (end-start+hourMs-1)/hourMs)
	for i := range buckets {
		buckets[i].hour = start + int64(i)*hourMs
	}
	bucket := func(t int64) *hourAgg { return &buckets[(t-start)/hourMs] }

	pts := samples
	if carry != nil {
		pts = append([]sample{*carry}, samples...)
	}
	for i, p := range pts {
		if p.ts >= start {
			b := bucket(p.ts)
			b.extend(p.num)
			b.psum += p.num
			b.n++
		}
		segEnd := p.ts + holdMs
		if i+1 < len(pts) {
			segEnd = min(segEnd, pts[i+1].ts)
		}
		segEnd = min(segEnd, end)
		for t := max(p.ts, start); t < segEnd; {
			b := bucket(t)
			e := min(b.hour+hourMs, segEnd)
			b.extend(p.num)
			b.wsum += p.num * float64(e-t)
			b.dur += e - t
			t = e
		}
	}

	out := buckets[:0]
	for _, b := range buckets {
		if b.seen {
			out = append(out, b)
		}
	}
	return out
}

func floorHour(ms int64) int64 {
	return ms - ms%hourMs
}
