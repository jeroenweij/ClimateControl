package store

import (
	"context"
	"database/sql"
)

// Floor is one uploaded floor-plan image.
type Floor struct {
	ID        int    `json:"id"`
	Name      string `json:"name"`
	ImagePath string `json:"-"`
	WidthPx   int    `json:"widthPx"`
	HeightPx  int    `json:"heightPx"`
	Sort      int    `json:"sort"`
}

// Placement is one ControllerNode's position on a floor.
type Placement struct {
	NodeID   int     `json:"nodeId"`
	FloorID  int     `json:"floorId"`
	XPx      int     `json:"xPx"`
	YPx      int     `json:"yPx"`
	PolyJSON *string `json:"polyJson,omitempty"`
}

// AddFloor inserts a floor and returns its id.
func (s *Store) AddFloor(ctx context.Context, name, imagePath string, w, h int) (int, error) {
	res, err := s.db.ExecContext(ctx,
		`INSERT INTO map_floors (name, image_path, width_px, height_px) VALUES (?, ?, ?, ?)`,
		name, imagePath, w, h)
	if err != nil {
		return 0, err
	}
	id, err := res.LastInsertId()
	return int(id), err
}

// Floors lists floors in display order.
func (s *Store) Floors(ctx context.Context) ([]Floor, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT id, name, image_path, width_px, height_px, sort FROM map_floors ORDER BY sort, id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Floor
	for rows.Next() {
		var f Floor
		if err := rows.Scan(&f.ID, &f.Name, &f.ImagePath, &f.WidthPx, &f.HeightPx, &f.Sort); err != nil {
			return nil, err
		}
		out = append(out, f)
	}
	return out, rows.Err()
}

// Floor returns one floor by id.
func (s *Store) Floor(ctx context.Context, id int) (Floor, error) {
	var f Floor
	err := s.db.QueryRowContext(ctx,
		`SELECT id, name, image_path, width_px, height_px, sort FROM map_floors WHERE id = ?`, id).
		Scan(&f.ID, &f.Name, &f.ImagePath, &f.WidthPx, &f.HeightPx, &f.Sort)
	return f, err
}

// DeleteFloor removes a floor and any placements on it.
func (s *Store) DeleteFloor(ctx context.Context, id int) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.ExecContext(ctx, `DELETE FROM map_placements WHERE floor_id = ?`, id); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, `DELETE FROM map_floors WHERE id = ?`, id); err != nil {
		return err
	}
	return tx.Commit()
}

// SetPlacement upserts one node's map position.
func (s *Store) SetPlacement(ctx context.Context, p Placement) error {
	var poly any
	if p.PolyJSON != nil {
		poly = *p.PolyJSON
	}
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO map_placements (node_id, floor_id, x_px, y_px, poly_json)
		VALUES (?, ?, ?, ?, ?)
		ON CONFLICT(node_id) DO UPDATE SET
			floor_id = excluded.floor_id, x_px = excluded.x_px,
			y_px = excluded.y_px, poly_json = excluded.poly_json`,
		p.NodeID, p.FloorID, p.XPx, p.YPx, poly)
	return err
}

// DeletePlacement removes one node from the map.
func (s *Store) DeletePlacement(ctx context.Context, nodeID int) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM map_placements WHERE node_id = ?`, nodeID)
	return err
}

// Placements lists every placed node.
func (s *Store) Placements(ctx context.Context) ([]Placement, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT node_id, floor_id, x_px, y_px, poly_json FROM map_placements ORDER BY node_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Placement
	for rows.Next() {
		var p Placement
		var poly sql.NullString
		if err := rows.Scan(&p.NodeID, &p.FloorID, &p.XPx, &p.YPx, &poly); err != nil {
			return nil, err
		}
		if poly.Valid {
			p.PolyJSON = &poly.String
		}
		out = append(out, p)
	}
	return out, rows.Err()
}
