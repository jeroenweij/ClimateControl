package nodelib

import "encoding/binary"

// Value is a decoded endpoint payload. Num holds a numeric reading (already
// scaled to engineering units, e.g. °C not centi-°C) when Kind == "number";
// Text holds a human string for structured/enumerated payloads. Fields holds
// the broken-out components for multi-field endpoints (SystemStatus, Diag*).
type Value struct {
	Kind   string         `json:"kind"` // number | enum | struct | raw
	Num    float64        `json:"num,omitempty"`
	Unit   string         `json:"unit,omitempty"`
	Text   string         `json:"text,omitempty"`
	Fields map[string]any `json:"fields,omitempty"`
}

func u16(d []byte) uint16 { return binary.LittleEndian.Uint16(d) }
func u32(d []byte) uint32 { return binary.LittleEndian.Uint32(d) }
func i16(d []byte) int16  { return int16(binary.LittleEndian.Uint16(d)) }

var damperModes = [...]string{"closed", "open", "auto", "manual", "stalled"}

func modeName(b byte) string {
	if int(b) < len(damperModes) {
		return damperModes[b]
	}
	return "mode(" + itoa(int(b)) + ")"
}

// DecodeValue interprets a Report/Set payload for an endpoint per
// Node-Message-Model-Spec.md §5. It never fails: an unrecognised or
// short payload comes back as Kind "raw".
func DecodeValue(e Endpoint, data []byte) Value {
	raw := Value{Kind: "raw", Text: hexBytes(data)}

	switch e {
	case EndpointSupplyTemp, EndpointReturnTemp, EndpointRoomTemp, EndpointRoomSetpoint:
		if len(data) < 2 {
			return raw
		}
		return Value{Kind: "number", Num: float64(i16(data)) / 100, Unit: "°C"}

	case EndpointRoomHumidity:
		if len(data) < 2 {
			return raw
		}
		return Value{Kind: "number", Num: float64(u16(data)) / 100, Unit: "%RH"}

	case EndpointDamperTarget, EndpointDamperActual:
		if len(data) < 1 {
			return raw
		}
		return Value{Kind: "number", Num: float64(data[0]), Unit: "%"}

	case EndpointDamperMode, EndpointRoomMode:
		if len(data) < 1 {
			return raw
		}
		return Value{Kind: "enum", Num: float64(data[0]), Text: modeName(data[0])}

	case EndpointRoomLink:
		if len(data) < 1 {
			return raw
		}
		return Value{Kind: "enum", Num: float64(data[0]), Text: boolWord(data[0] != 0, "up", "down")}

	case EndpointSensorStatus:
		if len(data) < 1 {
			return raw
		}
		return Value{Kind: "struct", Num: float64(data[0]), Fields: map[string]any{
			"returnValid": data[0]&0x01 != 0,
			"supplyValid": data[0]&0x02 != 0,
		}}

	case EndpointSystemInfo:
		if len(data) < 6 {
			return raw
		}
		return Value{Kind: "struct", Fields: map[string]any{
			"module":  Module(data[0]).String(),
			"hwRev":   data[1],
			"fwMajor": u16(data[2:]),
			"fwMinor": u16(data[4:]),
		}}

	case EndpointSystemStatus:
		if len(data) < 8 {
			return raw
		}
		return Value{Kind: "struct", Fields: map[string]any{
			"state":      data[0],
			"uptimeSec":  u32(data[1:]),
			"errorFlags": u16(data[5:]),
			"resetCause": data[7],
		}}

	case EndpointDiagRxCounters:
		if len(data) < 16 {
			return raw
		}
		return Value{Kind: "struct", Fields: map[string]any{
			"frames":           u32(data[0:]),
			"crcErrors":        u32(data[4:]),
			"resyncs":          u32(data[8:]),
			"interByteTimeout": u32(data[12:]),
		}}

	case EndpointDiagTxCounters:
		if len(data) < 8 {
			return raw
		}
		return Value{Kind: "struct", Fields: map[string]any{
			"txFrames":   u32(data[0:]),
			"queueDrops": u32(data[4:]),
		}}

	case EndpointDiagLastError:
		if len(data) < 7 {
			return raw
		}
		return Value{Kind: "struct", Fields: map[string]any{
			"code":          data[0],
			"uptimeAtFault": u32(data[1:]),
			"context":       u16(data[5:]),
		}}
	}
	return raw
}

// EncodeValue is the inverse used for downlink Set frames: it turns a numeric
// UI value into the endpoint's wire bytes. ok is false for endpoints that are
// not writable / not understood.
func EncodeValue(e Endpoint, num float64) (data []byte, ok bool) {
	switch e {
	case EndpointRoomSetpoint, EndpointRoomTemp, EndpointSupplyTemp, EndpointReturnTemp:
		v := int16(num * 100)
		return binary.LittleEndian.AppendUint16(nil, uint16(v)), true
	case EndpointDamperTarget:
		if num < 0 {
			num = 0
		}
		if num > 100 {
			num = 100
		}
		return []byte{byte(num)}, true
	case EndpointDamperMode, EndpointRoomMode, EndpointSystemControl:
		return []byte{byte(num)}, true
	}
	return nil, false
}

func boolWord(b bool, t, f string) string {
	if b {
		return t
	}
	return f
}

func hexBytes(d []byte) string {
	if len(d) == 0 {
		return ""
	}
	const hex = "0123456789abcdef"
	out := make([]byte, 0, len(d)*2)
	for _, b := range d {
		out = append(out, hex[b>>4], hex[b&0xF])
	}
	return string(out)
}
