package store

import (
	"context"
	"database/sql"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// FirmwareImage is one row of the firmware repository: the current image held
// for a module type.
type FirmwareImage struct {
	Module     string `json:"module"`
	Filename   string `json:"filename"`
	Version    int    `json:"version"` // major<<8 | minor
	VersionStr string `json:"versionStr"`
	Size       int    `json:"size"`
	CRC32      uint32 `json:"crc32"`
	UploadedTS int64  `json:"uploadedTs"`
	ImagePath  string `json:"-"`
}

// PutFirmware upserts the image held for a module. The caller is responsible
// for writing image_path and for deleting any file the previous row pointed at
// (use FirmwareImage first to find it).
func (s *Store) PutFirmware(ctx context.Context, mod nodelib.Module, filename string, version, size int, crc uint32, imagePath string) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO firmware_images (module, filename, version, size, crc32, uploaded_ts, image_path)
		VALUES (?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(module) DO UPDATE SET
			filename    = excluded.filename,
			version     = excluded.version,
			size        = excluded.size,
			crc32       = excluded.crc32,
			uploaded_ts = excluded.uploaded_ts,
			image_path  = excluded.image_path`,
		int(mod), filename, version, size, int64(crc), time.Now().UnixMilli(), imagePath)
	return err
}

// FirmwareImage returns the image held for one module (ok == false when none).
func (s *Store) FirmwareImage(ctx context.Context, mod nodelib.Module) (FirmwareImage, bool, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT module, filename, version, size, crc32, uploaded_ts, image_path
		 FROM firmware_images WHERE module = ?`, int(mod))
	fi, err := scanFirmware(row)
	if err == sql.ErrNoRows {
		return FirmwareImage{}, false, nil
	}
	if err != nil {
		return FirmwareImage{}, false, err
	}
	return fi, true, nil
}

// FirmwareImages lists every held image, ascending by module.
func (s *Store) FirmwareImages(ctx context.Context) ([]FirmwareImage, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT module, filename, version, size, crc32, uploaded_ts, image_path
		 FROM firmware_images ORDER BY module`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []FirmwareImage
	for rows.Next() {
		fi, err := scanFirmware(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, fi)
	}
	return out, rows.Err()
}

// DeleteFirmware drops the row for a module and returns the file path it
// pointed at (so the caller can remove it), or "" if there was no row.
func (s *Store) DeleteFirmware(ctx context.Context, mod nodelib.Module) (string, error) {
	fi, ok, err := s.FirmwareImage(ctx, mod)
	if err != nil || !ok {
		return "", err
	}
	if _, err := s.db.ExecContext(ctx, `DELETE FROM firmware_images WHERE module = ?`, int(mod)); err != nil {
		return "", err
	}
	return fi.ImagePath, nil
}

func scanFirmware(row rowScanner) (FirmwareImage, error) {
	var fi FirmwareImage
	var mod int
	var crc int64
	if err := row.Scan(&mod, &fi.Filename, &fi.Version, &fi.Size, &crc, &fi.UploadedTS, &fi.ImagePath); err != nil {
		return FirmwareImage{}, err
	}
	fi.Module = nodelib.Module(mod).String()
	fi.CRC32 = uint32(crc)
	fi.VersionStr = nodelib.VersionString(fi.Version)
	return fi, nil
}
