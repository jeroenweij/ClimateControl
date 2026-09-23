package nodelib

import "encoding/binary"

// OtaControlOp mirrors Boot::Firmware::ControlOp (Software/Modules/
// MainBootloader/Firmware.h) -- the sub-opcode carried in data[0] of an
// OtaControl Set. OtaControl / OtaData (uplink block, MainController-Server-
// Link-Spec.md §5) are spoken only by MainController's own bootloader, never
// by the running app or a bus node; they drive MainController's self-update.
type OtaControlOp uint8

const (
	OtaOpBegin    OtaControlOp = 0x01
	OtaOpAbort    OtaControlOp = 0x02
	OtaOpEnd      OtaControlOp = 0x03
	OtaOpActivate OtaControlOp = 0x04
)

// SystemControlResetToBootloader is the SystemControl[Set] value that parks a
// running MainController in its bootloader (NODE = 0, handled by the app's
// UplinkHandler rather than relayed onto the bus).
const SystemControlResetToBootloader = 2

// OtaChunk is the most data bytes one OtaData Set carries -- a whole number of
// 8-byte flash double-words, so every chunk is independently programmable.
const OtaChunk = 32

// EncodeOtaBegin builds the 11-byte OtaControl[Begin] payload: op(1)
// imageSize(4 LE) imageCrc32(4 LE) fwVersion(2 LE).
func EncodeOtaBegin(imageSize, imageCRC32 uint32, fwVersion uint16) []byte {
	b := make([]byte, 0, 11)
	b = append(b, byte(OtaOpBegin))
	b = binary.LittleEndian.AppendUint32(b, imageSize)
	b = binary.LittleEndian.AppendUint32(b, imageCRC32)
	b = binary.LittleEndian.AppendUint16(b, fwVersion)
	return b
}

// EncodeOtaAbort builds a single-byte OtaControl[Abort] payload.
func EncodeOtaAbort() []byte { return []byte{byte(OtaOpAbort)} }

// EncodeOtaEnd builds a single-byte OtaControl[End] payload.
func EncodeOtaEnd() []byte { return []byte{byte(OtaOpEnd)} }

// EncodeOtaActivate builds a single-byte OtaControl[Activate] payload.
func EncodeOtaActivate() []byte { return []byte{byte(OtaOpActivate)} }

// EncodeOtaData builds an OtaData[Set] payload: byteOffset(2 LE) bytes(<=32).
func EncodeOtaData(offset uint16, chunk []byte) []byte {
	if len(chunk) > OtaChunk {
		chunk = chunk[:OtaChunk]
	}
	b := make([]byte, 0, 2+len(chunk))
	b = binary.LittleEndian.AppendUint16(b, offset)
	return append(b, chunk...)
}

// ParseOtaStatusReport decodes the OtaControl Report sent in answer to an
// OtaControl Get: state(1) expectedOffset(4 LE) lastError(1) fwVersion(2 LE).
// It is the same information as a bus FirmwareStatusReport (same State /
// error numbering), just without the leading op byte.
func ParseOtaStatusReport(data []byte) (FirmwareStatusReport, bool) {
	if len(data) < 8 {
		return FirmwareStatusReport{}, false
	}
	return FirmwareStatusReport{
		State:          data[0],
		ExpectedOffset: u32(data[1:]),
		LastError:      data[5],
		FWVersion:      u16(data[6:]),
	}, true
}

// An OtaControl Ack/Nack (Begin/End/Abort) is a FirmwareOpReply and an OtaData
// Ack/Nack is a FirmwareWriteReply -- identical payload shapes to the bus
// Firmware protocol; use ParseFirmwareOpReply / ParseFirmwareWriteReply.
