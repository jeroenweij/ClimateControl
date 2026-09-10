package nodelib

import (
	"encoding/binary"
	"errors"
)

// Frame is one decoded protocol message.
type Frame struct {
	Node      uint8
	Endpoint  Endpoint
	Operation Operation
	Data      []byte
}

var sync = [2]byte{0xEE, 0x42}

// ErrFrameTooLong is returned by Encode when Data exceeds MaxData.
var ErrFrameTooLong = errors.New("nodelib: frame DATA exceeds MaxData")

// Encode serialises a frame to the wire format:
//
//	SYNC(0xEE 0x42) LEN NODE ENDPOINT OPERATION DATA[LEN] CRC16(LE)
//
// The CRC covers NODE..DATA, matching Software/Lib/NodeLib/Frame.cpp.
func Encode(f Frame) ([]byte, error) {
	if len(f.Data) > MaxData {
		return nil, ErrFrameTooLong
	}
	body := make([]byte, 3+len(f.Data))
	body[0] = f.Node
	body[1] = byte(f.Endpoint)
	body[2] = byte(f.Operation)
	copy(body[3:], f.Data)

	crc := CRC16(body)
	out := make([]byte, 0, 2+1+len(body)+2)
	out = append(out, sync[0], sync[1], byte(len(f.Data)))
	out = append(out, body...)
	out = binary.LittleEndian.AppendUint16(out, crc)
	return out, nil
}

// Counters tallies deframer health, surfaced to the UI alongside MainStatus.
type Counters struct {
	Frames    uint64 `json:"frames"`
	CRCErrors uint64 `json:"crcErrors"`
	Resyncs   uint64 `json:"resyncs"`
}

type deframeState int

const (
	stSync0 deframeState = iota
	stSync1
	stLen
	stHeader
	stData
	stCRC0
	stCRC1
)

// Deframer is a streaming frame parser. Feed bytes from the TCP socket; it
// mirrors the state machine in Frame.cpp (a lost sync just resynchronises).
// Not safe for concurrent use.
type Deframer struct {
	Counters Counters

	state       deframeState
	length      int
	header      [3]byte
	headerIndex int
	data        []byte
	dataIndex   int
	recvCRC     uint16
}

// NewDeframer returns a ready parser.
func NewDeframer() *Deframer { return &Deframer{state: stSync0} }

func (d *Deframer) reset() {
	d.state = stSync0
	d.headerIndex = 0
	d.dataIndex = 0
}

// Push feeds one chunk and returns every complete, CRC-valid frame it yields.
// The returned frames own their Data (safe to retain).
func (d *Deframer) Push(chunk []byte) []Frame {
	var out []Frame
	for _, b := range chunk {
		if f, ok := d.step(b); ok {
			out = append(out, f)
		}
	}
	return out
}

func (d *Deframer) step(b byte) (Frame, bool) {
	switch d.state {
	case stSync0:
		if b == sync[0] {
			d.state = stSync1
		}
	case stSync1:
		switch b {
		case sync[1]:
			d.state = stLen
		case sync[0]:
			// stay
		default:
			d.state = stSync0
		}
	case stLen:
		if int(b) > MaxData {
			d.Counters.Resyncs++
			d.reset()
			break
		}
		d.length = int(b)
		d.headerIndex = 0
		d.dataIndex = 0
		d.data = make([]byte, d.length)
		d.state = stHeader
	case stHeader:
		d.header[d.headerIndex] = b
		d.headerIndex++
		if d.headerIndex == 3 {
			if d.length == 0 {
				d.state = stCRC0
			} else {
				d.state = stData
			}
		}
	case stData:
		d.data[d.dataIndex] = b
		d.dataIndex++
		if d.dataIndex == d.length {
			d.state = stCRC0
		}
	case stCRC0:
		d.recvCRC = uint16(b)
		d.state = stCRC1
	case stCRC1:
		d.recvCRC |= uint16(b) << 8
		body := make([]byte, 0, 3+d.length)
		body = append(body, d.header[0], d.header[1], d.header[2])
		body = append(body, d.data...)
		d.reset()
		if CRC16(body) != d.recvCRC {
			d.Counters.CRCErrors++
			return Frame{}, false
		}
		d.Counters.Frames++
		return Frame{
			Node:      d.header[0],
			Endpoint:  Endpoint(d.header[1]),
			Operation: Operation(d.header[2]),
			Data:      d.data,
		}, true
	}
	return Frame{}, false
}
