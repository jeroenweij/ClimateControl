package nodelib

import "encoding/binary"

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

// OtaControlSet is the server -> MC start/abort payload for 0x65.
type OtaControlSet struct {
	TargetNodeID uint8
	ImageSize    uint32
	ImageCRC32   uint32
	FWVersion    uint16
	Module       Module
}

// Encode returns the 12-byte 0x65 Set payload.
func (o OtaControlSet) Encode() []byte {
	b := make([]byte, 0, 12)
	b = append(b, o.TargetNodeID)
	b = binary.LittleEndian.AppendUint32(b, o.ImageSize)
	b = binary.LittleEndian.AppendUint32(b, o.ImageCRC32)
	b = binary.LittleEndian.AppendUint16(b, o.FWVersion)
	b = append(b, byte(o.Module))
	return b
}

// OtaControlReport is the MC -> server progress payload for 0x65.
type OtaControlReport struct {
	State        uint8  `json:"state"`
	TargetNodeID uint8  `json:"targetNodeId"`
	NextOffset   uint32 `json:"nextOffset"`
	LastError    uint8  `json:"lastError"`
}

// ParseOtaControlReport decodes a 0x65 Report payload.
func ParseOtaControlReport(data []byte) (OtaControlReport, bool) {
	if len(data) < 7 {
		return OtaControlReport{}, false
	}
	return OtaControlReport{
		State:        data[0],
		TargetNodeID: data[1],
		NextOffset:   u32(data[2:]),
		LastError:    data[6],
	}, true
}

// EncodeOtaData builds a 0x66 Set payload: offset(4 LE) then up to 27 bytes.
func EncodeOtaData(offset uint32, chunk []byte) []byte {
	if len(chunk) > 27 {
		chunk = chunk[:27]
	}
	b := binary.LittleEndian.AppendUint32(nil, offset)
	return append(b, chunk...)
}
