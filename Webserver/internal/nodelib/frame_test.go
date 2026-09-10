package nodelib

import (
	"bytes"
	"testing"
)

func TestCRC16CheckValue(t *testing.T) {
	// Canonical CRC-16/CCITT-FALSE check value for "123456789".
	if got := CRC16([]byte("123456789")); got != 0x29B1 {
		t.Fatalf("CRC16 check = 0x%04X, want 0x29B1", got)
	}
}

func TestEncodeDeframeRoundTrip(t *testing.T) {
	cases := []Frame{
		{Node: 5, Endpoint: EndpointRoomTemp, Operation: OpReport, Data: []byte{0x5F, 0x08}},
		{Node: 0, Endpoint: EndpointRoster, Operation: OpReport, Data: bytes.Repeat([]byte{0xAB}, MaxData)},
		{Node: 3, Endpoint: EndpointSystemInfo, Operation: OpGet, Data: nil},
	}
	d := NewDeframer()
	for _, want := range cases {
		raw, err := Encode(want)
		if err != nil {
			t.Fatalf("Encode(%+v): %v", want, err)
		}
		frames := d.Push(raw)
		if len(frames) != 1 {
			t.Fatalf("Push yielded %d frames, want 1", len(frames))
		}
		got := frames[0]
		if got.Node != want.Node || got.Endpoint != want.Endpoint || got.Operation != want.Operation || !bytes.Equal(got.Data, want.Data) {
			t.Fatalf("round trip: got %+v want %+v", got, want)
		}
	}
	if d.Counters.Frames != uint64(len(cases)) {
		t.Fatalf("Frames counter = %d, want %d", d.Counters.Frames, len(cases))
	}
}

func TestDeframerResync(t *testing.T) {
	good := Frame{Node: 7, Endpoint: EndpointDamperActual, Operation: OpReport, Data: []byte{42}}
	raw, _ := Encode(good)

	d := NewDeframer()
	// Junk, a truncated sync, then the real frame split across two Push calls.
	stream := append([]byte{0x00, 0x11, 0xEE, 0x99, 0xEE, 0x42, 0x7F}, raw...)
	d.Push(stream[:5])
	frames := d.Push(stream[5:])
	if len(frames) != 1 || frames[0].Data[0] != 42 {
		t.Fatalf("resync failed: %+v", frames)
	}
}

func TestDeframerRejectsBadCRC(t *testing.T) {
	raw, _ := Encode(Frame{Node: 1, Endpoint: EndpointRoomTemp, Operation: OpReport, Data: []byte{1, 2}})
	raw[len(raw)-1] ^= 0xFF // corrupt the CRC high byte
	d := NewDeframer()
	if frames := d.Push(raw); len(frames) != 0 {
		t.Fatalf("expected drop, got %+v", frames)
	}
	if d.Counters.CRCErrors != 1 {
		t.Fatalf("CRCErrors = %d, want 1", d.Counters.CRCErrors)
	}
}

func TestDecodeTemperature(t *testing.T) {
	v := DecodeValue(EndpointRoomTemp, []byte{0x5F, 0x08}) // 2143 -> 21.43
	if v.Kind != "number" || v.Num != 21.43 || v.Unit != "°C" {
		t.Fatalf("decode temp = %+v", v)
	}
	neg := DecodeValue(EndpointSupplyTemp, []byte{0x9C, 0xFF}) // -100 -> -1.00
	if neg.Num != -1.0 {
		t.Fatalf("decode negative temp = %+v", neg)
	}
}

func TestEncodeValueSetpoint(t *testing.T) {
	d, ok := EncodeValue(EndpointRoomSetpoint, 20.5)
	if !ok || !bytes.Equal(d, []byte{0x02, 0x08}) { // 2050 = 0x0802
		t.Fatalf("EncodeValue setpoint = %x ok=%v", d, ok)
	}
}
