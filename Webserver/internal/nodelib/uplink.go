package nodelib

// Helpers for the 0x60 uplink endpoint block
// (MainController-Server-Link-Spec.md §5). These frames travel only on the
// MainController <-> server socket, never on the RS485 bus.

// UplinkHello is the first frame after every (re)connect.
type UplinkHello struct {
	FWVersion uint16
	UptimeSec uint32
	NodeCount uint8
	AuthToken [16]byte
}

// ParseUplinkHello decodes a 0x60 Report payload.
func ParseUplinkHello(data []byte) (UplinkHello, bool) {
	if len(data) < 23 {
		return UplinkHello{}, false
	}
	var h UplinkHello
	h.FWVersion = u16(data[0:])
	h.UptimeSec = u32(data[2:])
	h.NodeCount = data[6]
	copy(h.AuthToken[:], data[7:23])
	return h, true
}

// RosterEntry is one node's row in a 0x61 Report (one entry per frame,
// Node == 0xFF terminates the stream).
type RosterEntry struct {
	NodeID     uint8
	Module     Module
	State      uint8
	LastSeenMs uint32
}

// ParseRosterEntry decodes a 0x61 Report payload.
func ParseRosterEntry(data []byte) (RosterEntry, bool) {
	if len(data) < 7 {
		return RosterEntry{}, false
	}
	return RosterEntry{
		NodeID:     data[0],
		Module:     Module(data[1]),
		State:      data[2],
		LastSeenMs: u32(data[3:]),
	}, true
}

// NodePresence is a 0x62 Report: a node joined (Up) or dropped.
type NodePresence struct {
	NodeID uint8
	Module Module
	Up     bool
}

// ParseNodePresence decodes a 0x62 Report payload.
func ParseNodePresence(data []byte) (NodePresence, bool) {
	if len(data) < 3 {
		return NodePresence{}, false
	}
	return NodePresence{NodeID: data[0], Module: Module(data[1]), Up: data[2] != 0}, true
}

// ThermostatStatus is a 0x63 Report: the presence, bootloader state, running
// firmware version and identity of the Thermostat paired to one ControllerNode,
// relayed by the MainController from Get RoomLink + Get ThermostatFirmware.
// See ControllerNode-Thermostat-Link-Spec.md §5.6.
type ThermostatStatus struct {
	ControllerNodeID uint8
	LinkUp           bool
	BLState          uint8
	FWMajor          uint8
	FWMinor          uint8
	UID              [12]byte
}

// FWVersion packs the running version as major<<8 | minor, matching the
// encoding stored for bus nodes.
func (t ThermostatStatus) FWVersion() int { return int(t.FWMajor)<<8 | int(t.FWMinor) }

// ParseThermostatStatus decodes a 0x63 Report payload (17 bytes).
func ParseThermostatStatus(data []byte) (ThermostatStatus, bool) {
	if len(data) < 17 {
		return ThermostatStatus{}, false
	}
	var t ThermostatStatus
	t.ControllerNodeID = data[0]
	t.LinkUp = data[1] != 0
	t.BLState = data[2]
	t.FWMajor = data[3]
	t.FWMinor = data[4]
	copy(t.UID[:], data[5:17])
	return t, true
}

// MainStatus is a 0x67 Report: MainController + bus health, ~10 s.
type MainStatus struct {
	RxFrames      uint32 `json:"rxFrames"`
	CRCErrors     uint32 `json:"crcErrors"`
	Resyncs       uint32 `json:"resyncs"`
	TxDrops       uint32 `json:"txDrops"`
	DownlinkDrops uint32 `json:"downlinkDrops"`
	WifiRSSI      int8   `json:"wifiRssi"`
	FreeHeap      uint16 `json:"freeHeap"`
}

// ParseMainStatus decodes a 0x67 Report payload.
func ParseMainStatus(data []byte) (MainStatus, bool) {
	if len(data) < 23 {
		return MainStatus{}, false
	}
	return MainStatus{
		RxFrames:      u32(data[0:]),
		CRCErrors:     u32(data[4:]),
		Resyncs:       u32(data[8:]),
		TxDrops:       u32(data[12:]),
		DownlinkDrops: u32(data[16:]),
		WifiRSSI:      int8(data[20]),
		FreeHeap:      u16(data[21:]),
	}, true
}
