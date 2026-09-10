// Package ws is a minimal RFC 6455 WebSocket server, enough for the
// ClimateControl live channel: the server pushes JSON text frames and answers
// client pings. No compression, no extensions, no fragmentation on send.
package ws

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const acceptMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// Conn is an accepted WebSocket connection. Writes are serialised; a single
// reader (ReadLoop) is assumed.
type Conn struct {
	nc     net.Conn
	br     *bufio.Reader
	wmu    sync.Mutex
	closed bool
}

// Accept performs the upgrade handshake and hijacks the connection.
func Accept(w http.ResponseWriter, r *http.Request) (*Conn, error) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") ||
		!headerContainsToken(r.Header.Get("Connection"), "upgrade") ||
		r.Header.Get("Sec-WebSocket-Version") != "13" {
		http.Error(w, "expected websocket upgrade", http.StatusBadRequest)
		return nil, errors.New("ws: not an upgrade request")
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	if key == "" {
		http.Error(w, "missing Sec-WebSocket-Key", http.StatusBadRequest)
		return nil, errors.New("ws: missing key")
	}
	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "no hijack", http.StatusInternalServerError)
		return nil, errors.New("ws: ResponseWriter is not a Hijacker")
	}
	nc, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}

	sum := sha1.Sum([]byte(key + acceptMagic))
	accept := base64.StdEncoding.EncodeToString(sum[:])
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := brw.WriteString(resp); err != nil {
		nc.Close()
		return nil, err
	}
	if err := brw.Flush(); err != nil {
		nc.Close()
		return nil, err
	}
	return &Conn{nc: nc, br: brw.Reader}, nil
}

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA
)

// WriteText sends one unfragmented text frame.
func (c *Conn) WriteText(p []byte) error { return c.writeFrame(opText, p) }

func (c *Conn) writeFrame(opcode byte, payload []byte) error {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if c.closed {
		return errors.New("ws: connection closed")
	}
	var hdr [10]byte
	hdr[0] = 0x80 | opcode // FIN + opcode
	n := 2
	switch {
	case len(payload) < 126:
		hdr[1] = byte(len(payload))
	case len(payload) < 1<<16:
		hdr[1] = 126
		binary.BigEndian.PutUint16(hdr[2:], uint16(len(payload)))
		n = 4
	default:
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:], uint64(len(payload)))
		n = 10
	}
	_ = c.nc.SetWriteDeadline(time.Now().Add(10 * time.Second))
	defer c.nc.SetWriteDeadline(time.Time{})
	if _, err := c.nc.Write(hdr[:n]); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := c.nc.Write(payload); err != nil {
			return err
		}
	}
	return nil
}

// ReadLoop consumes client frames until the connection closes. It answers
// pings and discards data frames (the client is not expected to send data).
func (c *Conn) ReadLoop() error {
	for {
		op, payload, err := c.readFrame()
		if err != nil {
			return err
		}
		switch op {
		case opPing:
			_ = c.writeFrame(opPong, payload)
		case opClose:
			_ = c.writeFrame(opClose, nil)
			return nil
		}
	}
}

func (c *Conn) readFrame() (byte, []byte, error) {
	_ = c.nc.SetReadDeadline(time.Now().Add(120 * time.Second))
	var h [2]byte
	if _, err := io.ReadFull(c.br, h[:]); err != nil {
		return 0, nil, err
	}
	opcode := h[0] & 0x0F
	masked := h[1]&0x80 != 0
	length := uint64(h[1] & 0x7F)
	switch length {
	case 126:
		var ext [2]byte
		if _, err := io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err := io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(ext[:])
	}
	if length > 1<<20 {
		return 0, nil, errors.New("ws: client frame too large")
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(c.br, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(c.br, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i&3]
		}
	}
	_ = opContinuation
	_ = opBinary
	return opcode, payload, nil
}

// Close sends a close frame and drops the connection.
func (c *Conn) Close() error {
	c.wmu.Lock()
	already := c.closed
	c.closed = true
	c.wmu.Unlock()
	if !already {
		_ = c.rawWrite([]byte{0x80 | opClose, 0})
	}
	return c.nc.Close()
}

func (c *Conn) rawWrite(b []byte) error {
	_ = c.nc.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, err := c.nc.Write(b)
	return err
}

func headerContainsToken(header, token string) bool {
	for _, part := range strings.Split(header, ",") {
		if strings.EqualFold(strings.TrimSpace(part), token) {
			return true
		}
	}
	return false
}
