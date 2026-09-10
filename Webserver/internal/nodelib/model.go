// Package nodelib implements the server side of the ClimateControl RS485 v2
// wire protocol: the frame codec (SYNC/LEN/header/DATA/CRC16) and the endpoint
// payload decoders. It mirrors Software/Lib/NodeLib and the specs
// RS485-Node-Protocol-Spec-STM32G030.md (§3 frame) and
// Node-Message-Model-Spec.md (§3 endpoints, §4 operations, §5 encodings).
package nodelib

// MaxData is the DATA-field cap enforced on the bus and this link.
const MaxData = 32

// NodeBroadcast is the reserved broadcast address (Set-only, no reply).
const NodeBroadcast = 0xFF

// NodeMaster is the reserved bus-master / MainController address.
const NodeMaster = 0

// Endpoint is the addressable thing on a node (frame byte 4).
type Endpoint uint8

const (
	EndpointTransport Endpoint = 0x00

	EndpointSystemInfo    Endpoint = 0x10
	EndpointSystemStatus  Endpoint = 0x11
	EndpointSystemControl Endpoint = 0x12

	EndpointFirmware Endpoint = 0x20

	EndpointDamperTarget Endpoint = 0x30
	EndpointDamperActual Endpoint = 0x31
	EndpointDamperMode   Endpoint = 0x32

	EndpointSupplyTemp   Endpoint = 0x38
	EndpointReturnTemp   Endpoint = 0x39
	EndpointSensorStatus Endpoint = 0x3A

	EndpointRoomSetpoint Endpoint = 0x40
	EndpointRoomTemp     Endpoint = 0x41
	EndpointRoomHumidity Endpoint = 0x42
	EndpointRoomMode     Endpoint = 0x43
	EndpointRoomLink     Endpoint = 0x44

	EndpointDiagRxCounters Endpoint = 0x50
	EndpointDiagTxCounters Endpoint = 0x51
	EndpointDiagLastError  Endpoint = 0x52
	EndpointDiagLog        Endpoint = 0x53
	EndpointDiagReset      Endpoint = 0x54

	// Uplink block (0x60-0x6F): MainController <-> server only, never on the
	// RS485 bus. See MainController-Server-Link-Spec.md §5.
	EndpointUplinkHello  Endpoint = 0x60
	EndpointRoster       Endpoint = 0x61
	EndpointNodePresence Endpoint = 0x62
	EndpointKeepalive    Endpoint = 0x64
	EndpointOtaControl   Endpoint = 0x65
	EndpointOtaData      Endpoint = 0x66
	EndpointMainStatus   Endpoint = 0x67
)

var endpointNames = map[Endpoint]string{
	EndpointTransport:      "Transport",
	EndpointSystemInfo:     "SystemInfo",
	EndpointSystemStatus:   "SystemStatus",
	EndpointSystemControl:  "SystemControl",
	EndpointFirmware:       "Firmware",
	EndpointDamperTarget:   "DamperTarget",
	EndpointDamperActual:   "DamperActual",
	EndpointDamperMode:     "DamperMode",
	EndpointSupplyTemp:     "SupplyTemp",
	EndpointReturnTemp:     "ReturnTemp",
	EndpointSensorStatus:   "SensorStatus",
	EndpointRoomSetpoint:   "RoomSetpoint",
	EndpointRoomTemp:       "RoomTemp",
	EndpointRoomHumidity:   "RoomHumidity",
	EndpointRoomMode:       "RoomMode",
	EndpointRoomLink:       "RoomLink",
	EndpointDiagRxCounters: "DiagRxCounters",
	EndpointDiagTxCounters: "DiagTxCounters",
	EndpointDiagLastError:  "DiagLastError",
	EndpointDiagLog:        "DiagLog",
	EndpointDiagReset:      "DiagReset",
	EndpointUplinkHello:    "UplinkHello",
	EndpointRoster:         "Roster",
	EndpointNodePresence:   "NodePresence",
	EndpointKeepalive:      "Keepalive",
	EndpointOtaControl:     "OtaControl",
	EndpointOtaData:        "OtaData",
	EndpointMainStatus:     "MainStatus",
}

var endpointByName = func() map[string]Endpoint {
	m := make(map[string]Endpoint, len(endpointNames))
	for e, n := range endpointNames {
		m[n] = e
	}
	return m
}()

func (e Endpoint) String() string {
	if n, ok := endpointNames[e]; ok {
		return n
	}
	return "Endpoint(0x" + hexByte(uint8(e)) + ")"
}

// EndpointByName resolves a name (as produced by Endpoint.String) back to the
// value; the second result is false for an unknown name.
func EndpointByName(name string) (Endpoint, bool) {
	e, ok := endpointByName[name]
	return e, ok
}

// Block returns the high nibble that groups the endpoint (0x10, 0x30, ...).
func (e Endpoint) Block() uint8 { return uint8(e) & 0xF0 }

// Operation is the verb (frame byte 5).
type Operation uint8

const (
	OpGet    Operation = 0x01
	OpSet    Operation = 0x02
	OpReport Operation = 0x03
	OpAck    Operation = 0x04
	OpNack   Operation = 0x05

	OpDiscover Operation = 0x10
	OpAnnounce Operation = 0x11
	OpPoll     Operation = 0x12
	OpDone     Operation = 0x13
)

var operationNames = map[Operation]string{
	OpGet: "Get", OpSet: "Set", OpReport: "Report", OpAck: "Ack", OpNack: "Nack",
	OpDiscover: "Discover", OpAnnounce: "Announce", OpPoll: "Poll", OpDone: "Done",
}

func (o Operation) String() string {
	if n, ok := operationNames[o]; ok {
		return n
	}
	return "Operation(0x" + hexByte(uint8(o)) + ")"
}

// Module identifies a board type (ConfigStore::Module / ImageModule).
type Module uint8

const (
	ModuleUnknown         Module = 0
	ModuleControllerNode  Module = 1
	ModuleTemperatureNode Module = 2
	ModuleMainController  Module = 3
	ModuleThermostat      Module = 4
)

var moduleNames = map[Module]string{
	ModuleUnknown:         "Unknown",
	ModuleControllerNode:  "ControllerNode",
	ModuleTemperatureNode: "TemperatureNode",
	ModuleMainController:  "MainController",
	ModuleThermostat:      "Thermostat",
}

func (m Module) String() string {
	if n, ok := moduleNames[m]; ok {
		return n
	}
	return "Module(" + itoa(int(m)) + ")"
}

var moduleByName = func() map[string]Module {
	m := make(map[string]Module, len(moduleNames))
	for mod, n := range moduleNames {
		m[n] = mod
	}
	return m
}()

// ModuleByName resolves a name (as produced by Module.String) back to the
// value; the second result is false for an unknown name.
func ModuleByName(name string) (Module, bool) {
	m, ok := moduleByName[name]
	return m, ok
}

func hexByte(b uint8) string {
	const hex = "0123456789ABCDEF"
	return string([]byte{hex[b>>4], hex[b&0xF]})
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [12]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}
