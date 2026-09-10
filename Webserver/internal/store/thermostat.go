package store

import (
	"context"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// Thermostat mirrors the link state of the Thermostat paired to one
// ControllerNode, from the 0x63 ThermostatStatus uplink report.
type Thermostat struct {
	ControllerNodeID int    `json:"controllerNodeId"`
	UID              string `json:"uid"` // hex, factory device id
	FWVersion        int    `json:"fwVersion"`
	FWVersionStr     string `json:"fwVersionStr"`
	BLState          int    `json:"blState"`
	LinkUp           bool   `json:"linkUp"`
	LastSeen         int64  `json:"lastSeen"`
}

// UpsertThermostat records a ThermostatStatus sighting for one ControllerNode.
func (s *Store) UpsertThermostat(ctx context.Context, t nodelib.ThermostatStatus) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO thermostats (controller_node_id, uid, fw_version, bl_state, link_up, last_seen)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(controller_node_id) DO UPDATE SET
			uid        = CASE WHEN length(excluded.uid) > 0 THEN excluded.uid ELSE thermostats.uid END,
			fw_version = CASE WHEN excluded.fw_version != 0 THEN excluded.fw_version ELSE thermostats.fw_version END,
			bl_state   = excluded.bl_state,
			link_up    = excluded.link_up,
			last_seen  = excluded.last_seen`,
		int(t.ControllerNodeID), t.UID[:], t.FWVersion(), int(t.BLState),
		boolInt(t.LinkUp), time.Now().Unix())
	return err
}

// Thermostats lists every known Thermostat, keyed by its ControllerNode id.
func (s *Store) Thermostats(ctx context.Context) (map[int]Thermostat, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT controller_node_id, uid, fw_version, bl_state, link_up, last_seen FROM thermostats`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make(map[int]Thermostat)
	for rows.Next() {
		var t Thermostat
		var uid []byte
		var linkUp int
		if err := rows.Scan(&t.ControllerNodeID, &uid, &t.FWVersion, &t.BLState, &linkUp, &t.LastSeen); err != nil {
			return nil, err
		}
		t.UID = hexBytes(uid)
		t.LinkUp = linkUp != 0
		t.FWVersionStr = nodelib.VersionString(t.FWVersion)
		out[t.ControllerNodeID] = t
	}
	return out, rows.Err()
}

func hexBytes(b []byte) string {
	const hex = "0123456789abcdef"
	out := make([]byte, 0, len(b)*2)
	for _, c := range b {
		out = append(out, hex[c>>4], hex[c&0xF])
	}
	return string(out)
}
