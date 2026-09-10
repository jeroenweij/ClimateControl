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
	State      string `json:"state"` // pending|entering|erasing|writing|verifying|done|error
	LastOffset int    `json:"lastOffset"`
	Error      string `json:"error,omitempty"`
	ImagePath  string `json:"-"`
}

// CreateOtaJob records a new firmware push.
func (s *Store) CreateOtaJob(ctx context.Context, j OtaJob) (int64, error) {
	target := j.Target
	if target == "" {
		target = "node"
	}
	res, err := s.db.ExecContext(ctx, `
		INSERT INTO ota_jobs (node_id, target, filename, size, crc32, fw_version, module, started, state, image_path)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'pending', ?)`,
		j.NodeID, target, j.Filename, j.Size, int64(j.CRC32), j.FWVersion, j.Module,
		time.Now().UnixMilli(), j.ImagePath)
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
	var out []OtaJob
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

const otaSelect = `SELECT id, node_id, target, filename, size, crc32, fw_version, module,
	started, finished, state, last_offset, error, image_path FROM ota_jobs`

type rowScanner interface {
	Scan(dest ...any) error
}

func scanOtaJob(row rowScanner) (OtaJob, error) {
	var j OtaJob
	var finished sql.NullInt64
	var errMsg sql.NullString
	var crc int64
	err := row.Scan(&j.ID, &j.NodeID, &j.Target, &j.Filename, &j.Size, &crc, &j.FWVersion, &j.Module,
		&j.Started, &finished, &j.State, &j.LastOffset, &errMsg, &j.ImagePath)
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
