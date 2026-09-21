package nodelib

import "testing"

func TestParseFirmwareOpReply(t *testing.T) {
	r, ok := ParseFirmwareOpReply(false, []byte{0})
	if !ok || r.Nack || r.LastError != 0 {
		t.Fatalf("Ack(0): got %+v ok=%v", r, ok)
	}

	r, ok = ParseFirmwareOpReply(true, []byte{FwErrCrcMismatch})
	if !ok || !r.Nack || r.LastError != FwErrCrcMismatch {
		t.Fatalf("Nack(CrcMismatch): got %+v ok=%v", r, ok)
	}
}

// A Write reply (5 bytes: offset, chunkCrc16, programFailed) must never be
// mistaken for a Begin/End/Abort op reply (1 byte: lastError) -- both ride
// the same Operation Ack/Nack and neither carries a FirmwareOp sub-byte, so
// payload length is the only thing telling them apart.
func TestParseFirmwareOpReplyDoesNotMatchAWriteReply(t *testing.T) {
	writeReplyPayload := []byte{0x00, 0x01, 0xAB, 0xCD, 0x00} // offset=0x0100 crc=0xCDAB programFailed=0
	if _, ok := ParseFirmwareOpReply(false, writeReplyPayload); ok {
		t.Fatalf("5-byte write reply payload parsed as a 1-byte op reply")
	}
	if _, ok := ParseFirmwareWriteReply(false, []byte{0x2A}); ok {
		t.Fatalf("1-byte op reply payload parsed as a 5-byte write reply")
	}
}
