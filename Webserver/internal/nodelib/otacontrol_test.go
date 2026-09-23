package nodelib

import (
	"bytes"
	"testing"
)

// The byte layouts below are the contract with Software/Modules/
// MainBootloader/Firmware.cpp (BeginLen = 11, DataHeaderLen = 2,
// StatusLen = 8) -- Software/Modules/MainBootloader/test/FirmwareTests.cpp
// builds the same shapes by hand.
func TestEncodeOtaBegin(t *testing.T) {
	got := EncodeOtaBegin(0x00004E20, 0xDEADBEEF, 0x0203)
	want := []byte{0x01, 0x20, 0x4E, 0x00, 0x00, 0xEF, 0xBE, 0xAD, 0xDE, 0x03, 0x02}
	if !bytes.Equal(got, want) {
		t.Fatalf("got % X, want % X", got, want)
	}
}

func TestEncodeOtaControlOps(t *testing.T) {
	if !bytes.Equal(EncodeOtaAbort(), []byte{2}) || !bytes.Equal(EncodeOtaEnd(), []byte{3}) || !bytes.Equal(EncodeOtaActivate(), []byte{4}) {
		t.Fatal("op-only payloads wrong")
	}
}

func TestEncodeOtaData(t *testing.T) {
	chunk := make([]byte, 40)
	for i := range chunk {
		chunk[i] = byte(i)
	}
	got := EncodeOtaData(0x0120, chunk)
	if len(got) != 2+OtaChunk || got[0] != 0x20 || got[1] != 0x01 || got[2] != 0 || got[33] != 31 {
		t.Fatalf("got % X", got)
	}
	if len(got) > MaxData {
		t.Fatalf("payload %d exceeds MaxData", len(got))
	}
}

func TestParseOtaStatusReport(t *testing.T) {
	r, ok := ParseOtaStatusReport([]byte{BlReceiving, 0x40, 0x00, 0x00, 0x00, FwErrNone, 0x03, 0x02})
	if !ok || r.State != BlReceiving || r.ExpectedOffset != 0x40 || r.LastError != FwErrNone || r.FWVersion != 0x0203 {
		t.Fatalf("got %+v ok=%v", r, ok)
	}
	if _, ok := ParseOtaStatusReport(make([]byte, 7)); ok {
		t.Fatal("short report accepted")
	}
}
