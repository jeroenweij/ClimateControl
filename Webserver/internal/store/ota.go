package store

import (
	"context"
	"database/sql"
	"time"
)

// OtaJob tracks one firmware push.
type OtaJob struct {
	ID     int64 `json:"id"`
	NodeID int   `json:"nodeId"`
	// Target is "node" (the bus node itself) or "thermostat" (the Thermostat
	// paired to the ControllerNode at NodeID, pushed over its private link).
	Target     string `json:"target"`
	Filename   string `json:"filename"`
	Size       int    `json:"size"`
	CRC32      uint32 `json:"crc32"`
	FWVersion  int    `json:"fwVersion"`
	Module     int    `json:"module"`
	Started    int64  `json:"started"`
	Finished   *int64 `json:"finished,omitempty"`
	State      string `json:"state"` // queued|entering|erasing|writing|verifying|done|error
	LastOffset int    `json:"lastOffset"`
	Error      string `json:"error,omitempty"`
	ImagePath  string `json:"-"`
	// Force re-flashes a Thermostat even if it already runs this version --
	// sent as ThermostatFirmware[Begin] flags bit 0, which disables the
	// ControllerNode's already-current guard (link spec §5.4.1).
	Force bool `json:"force,omitempty"`
}

// CreateOtaJob records a new firmware push.
func (s *Store) CreateOtaJob(ctx context.Context, j OtaJob) (int64, error) {
	target := j.Target
	if target == "" {
		target = "node"
	}
	res, err := s.db.ExecContext(ctx, `
		INSERT INTO ota_jobs (node_id, target, filename, size, crc32, fw_version, module, started, state, image_path, force)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'queued', ?, ?)`,
		j.NodeID, target, j.Filename, j.Size, int64(j.CRC32), j.FWVersion, j.Module,
		time.Now().UnixMilli(), j.ImagePath, j.Force)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// UpdateOtaJob writes progress. Pass state "" to leave it unchanged.
func (s *Store) UpdateOtaJob(ctx context.Context, id int64, state string, lastOffset int, errMsg string) error {
	var finished any
	if state == "done" || state == "error" {
		finished = time.Now().UnixMilli()
	}
	_, err := s.db.ExecContext(ctx, `
		UPDATE ota_jobs SET
			state       = CASE WHEN ?1 != '' THEN ?1 ELSE state END,
			last_offset = ?2,
			error       = CASE WHEN ?3 != '' THEN ?3 ELSE error END,
			finished    = COALESCE(?4, finished)
		WHERE id = ?5`,
		state, lastOffset, errMsg, finished, id)
	return err
}

// OtaJobsKept is how many push records are retained; older finished ones are
// pruned (PruneOtaJobs).
const OtaJobsKept = 100

// PruneOtaJobs deletes finished (done/error) jobs older than the newest
// OtaJobsKept records and returns their image paths so the caller can remove
// the files. A queued or running job is never deleted, however old.
func (s *Store) PruneOtaJobs(ctx context.Context) ([]string, error) {
	var cutoff int64
	err := s.db.QueryRowContext(ctx,
		`SELECT id FROM ota_jobs ORDER BY id DESC LIMIT 1 OFFSET ?`, OtaJobsKept-1).Scan(&cutoff)
	if err == sql.ErrNoRows {
		return nil, nil // fewer than OtaJobsKept records
	}
	if err != nil {
		return nil, err
	}

	rows, err := s.db.QueryContext(ctx,
		`SELECT image_path FROM ota_jobs WHERE id < ? AND state IN ('done','error')`, cutoff)
	if err != nil {
		return nil, err
	}
	var paths []string
	for rows.Next() {
		var p sql.NullString
		if err := rows.Scan(&p); err != nil {
			rows.Close()
			return nil, err
		}
		if p.String != "" {
			paths = append(paths, p.String)
		}
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	_, err = s.db.ExecContext(ctx, `DELETE FROM ota_jobs WHERE id < ? AND state IN ('done','error')`, cutoff)
	return paths, err
}

// OtaJob returns one job.
func (s *Store) OtaJob(ctx context.Context, id int64) (OtaJob, error) {
	return scanOtaJob(s.db.QueryRowContext(ctx, otaSelect+` WHERE id = ?`, id))
}

// OtaJobs lists jobs newest first.
func (s *Store) OtaJobs(ctx context.Context, limit int) ([]OtaJob, error) {
	if limit <= 0 {
		limit = 50
	}
	rows, err := s.db.QueryContext(ctx, otaSelect+` ORDER BY id DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []OtaJob{}
	for rows.Next() {
		j, err := scanOtaJob(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

// ActiveOtaJob returns the most recent job that is neither done nor error, if
// any (ok == false when none).
func (s *Store) ActiveOtaJob(ctx context.Context) (OtaJob, bool, error) {
	j, err := scanOtaJob(s.db.QueryRowContext(ctx, otaSelect+
		` WHERE state NOT IN ('done','error') ORDER BY id DESC LIMIT 1`))
	if err == sql.ErrNoRows {
		return OtaJob{}, false, nil
	}
	if err != nil {
		return OtaJob{}, false, err
	}
	return j, true, nil
}

// NextQueuedOtaJob returns the oldest job still waiting to run (ok == false
// when the queue is empty). Only one push runs at a time; the driver moves a
// job out of 'queued' as soon as it starts.
func (s *Store) NextQueuedOtaJob(ctx context.Context) (OtaJob, bool, error) {
	j, err := scanOtaJob(s.db.QueryRowContext(ctx, otaSelect+
		` WHERE state = 'queued' ORDER BY id ASC LIMIT 1`))
	if err == sql.ErrNoRows {
		return OtaJob{}, false, nil
	}
	if err != nil {
		return OtaJob{}, false, err
	}
	return j, true, nil
}

// HasPendingOtaJob reports whether a job for this node+target is already queued
// or running, so a repeated operator press does not stack duplicates.
func (s *Store) HasPendingOtaJob(ctx context.Context, nodeID int, target string) (bool, error) {
	var n int
	err := s.db.QueryRowContext(ctx,
		`SELECT COUNT(*) FROM ota_jobs WHERE node_id = ? AND target = ? AND state NOT IN ('done','error')`,
		nodeID, target).Scan(&n)
	return n > 0, err
}

const otaSelect = `SELECT id, node_id, target, filename, size, crc32, fw_version, module,
	started, finished, state, last_offset, error, image_path, force FROM ota_jobs`

type rowScanner interface {
	Scan(dest ...any) error
}

func scanOtaJob(row rowScanner) (OtaJob, error) {
	var j OtaJob
	var finished sql.NullInt64
	var errMsg sql.NullString
	var crc int64
	err := row.Scan(&j.ID, &j.NodeID, &j.Target, &j.Filename, &j.Size, &crc, &j.FWVersion, &j.Module,
		&j.Started, &finished, &j.State, &j.LastOffset, &errMsg, &j.ImagePath, &j.Force)
	if err != nil {
		return OtaJob{}, err
	}
	j.CRC32 = uint32(crc)
	if finished.Valid {
		j.Finished = &finished.Int64
	}
	j.Error = errMsg.String
	return j, nil
}
