package nodelib

import "encoding/binary"

// FirmwareOp mirrors NodeLib::FirmwareOp (Software/Lib/NodeLib/EFirmware.h) --
// the sub-opcode carried in data[0] of a Firmware / ThermostatFirmware frame.
// Both endpoints are ordinary relayed bus frames (block 0x10-0x50); driving
// the OTA sequence is entirely the server's job, matching what a plain bus
// node's Endpoint::Firmware slave (and a ControllerNode's ThermostatFirmware
// relay) actually expect -- there is no separate uplink-only OTA protocol.
type FirmwareOp uint8

const (
	FirmwareOpBegin           FirmwareOp = 0x01
	FirmwareOpWrite           FirmwareOp = 0x02
	FirmwareOpEnd             FirmwareOp = 0x03
	FirmwareOpActivate        FirmwareOp = 0x04
	FirmwareOpAbort           FirmwareOp = 0x05
	FirmwareOpEnterBootloader FirmwareOp = 0x06
	FirmwareOpStatus          FirmwareOp = 0x07
)

// Bootloader states reported in a Status's state byte (Modules/Bootloader/
// FirmwareSlave.h's State enum -- private to that module, mirrored here).
// BlApp (0) is never sent by the bootloader itself, but a ControllerNode
// relaying ThermostatFirmware can report it for a Thermostat that never left
// app mode (the already-current guard, ControllerNode-Thermostat-Link-Spec.md
// §5.4.1).
const (
	BlApp       = 0
	BlIdle      = 1
	BlErasing   = 2
	BlReceiving = 3
	BlValid     = 4
	BlError     = 5
)

// FirmwareError mirrors NodeLib::FirmwareError (EFirmware.h).
const (
	FwErrNone           = 0
	FwErrWrongModule    = 1
	FwErrBadSize        = 2
	FwErrEraseFailed    = 3
	FwErrProgramFailed  = 4
	FwErrOverrun        = 5
	FwErrCrcMismatch    = 6
	FwErrBadState       = 7
	FwErrAlreadyCurrent = 8 // ThermostatFirmware only -- no-op update, skipped
	FwErrLinkDown       = 9 // ThermostatFirmware only -- Thermostat unreachable
)

// EncodeFirmwareBegin builds the 12-byte Firmware[Begin] payload: op(1)
// module(1) imageSize(4 LE) imageCrc32(4 LE) fwVersion(2 LE).
func EncodeFirmwareBegin(module Module, imageSize, imageCRC32 uint32, fwVersion uint16) []byte {
	b := make([]byte, 0, 12)
	b = append(b, byte(FirmwareOpBegin), byte(module))
	b = binary.LittleEndian.AppendUint32(b, imageSize)
	b = binary.LittleEndian.AppendUint32(b, imageCRC32)
	b = binary.LittleEndian.AppendUint16(b, fwVersion)
	return b
}

// EncodeThermostatFirmwareBegin builds the 13-byte ThermostatFirmware[Begin]
// payload: the same fields as EncodeFirmwareBegin plus a trailing flags byte
// (bit 0 = Force, ControllerNode-Thermostat-Link-Spec.md §5.4.1) -- with
// Force clear, the ControllerNode skips a no-op re-flash of an already-
// current Thermostat on its own.
func EncodeThermostatFirmwareBegin(imageSize, imageCRC32 uint32, fwVersion uint16, force bool) []byte {
	b := EncodeFirmwareBegin(ModuleThermostat, imageSize, imageCRC32, fwVersion)
	var flags byte
	if force {
		flags = 1
	}
	return append(b, flags)
}

// EncodeFirmwareWrite builds a Firmware[Write] (or ThermostatFirmware[Write])
// payload: op(1) offset(4 LE) bytes(<=27) -- identical shape either way.
func EncodeFirmwareWrite(offset uint32, chunk []byte) []byte {
	if len(chunk) > 27 {
		chunk = chunk[:27]
	}
	b := make([]byte, 0, 5+len(chunk))
	b = append(b, byte(FirmwareOpWrite))
	b = binary.LittleEndian.AppendUint32(b, offset)
	return append(b, chunk...)
}

func firmwareOpOnly(op FirmwareOp) []byte { return []byte{byte(op)} }

// EncodeFirmwareEnd builds a single-byte Firmware[End] / ThermostatFirmware[End] payload.
func EncodeFirmwareEnd() []byte { return firmwareOpOnly(FirmwareOpEnd) }

// EncodeFirmwareActivate builds a single-byte Firmware[Activate] / ThermostatFirmware[Activate] payload.
func EncodeFirmwareActivate() []byte { return firmwareOpOnly(FirmwareOpActivate) }

// EncodeFirmwareAbort builds a single-byte Firmware[Abort] / ThermostatFirmware[Abort] payload.
func EncodeFirmwareAbort() []byte { return firmwareOpOnly(FirmwareOpAbort) }

// EncodeFirmwareEnterBootloader builds a single-byte Firmware[EnterBootloader]
// payload -- only meaningful addressed to a bus node's own Firmware endpoint;
// ThermostatFirmware[Begin] does this step itself, CN-side.
func EncodeFirmwareEnterBootloader() []byte { return firmwareOpOnly(FirmwareOpEnterBootloader) }

// FirmwareStatusReport is a decoded Firmware / ThermostatFirmware Status
// report: op(1) state(1) expectedOffset(4 LE) lastError(1) fwVersion(2 LE).
type FirmwareStatusReport struct {
	State          uint8
	ExpectedOffset uint32
	LastError      uint8
	FWVersion      uint16
}

// ParseFirmwareStatusReport decodes a Firmware / ThermostatFirmware Report
// payload. ok is false for anything that isn't a Status report.
func ParseFirmwareStatusReport(data []byte) (FirmwareStatusReport, bool) {
	if len(data) < 9 || FirmwareOp(data[0]) != FirmwareOpStatus {
		return FirmwareStatusReport{}, false
	}
	return FirmwareStatusReport{
		State:          data[1],
		ExpectedOffset: u32(data[2:]),
		LastError:      data[6],
		FWVersion:      u16(data[7:]),
	}, true
}
