// Package uplink is the TCP server the MainController connects to. It
// authenticates the connection (UplinkHello token), deframes the stream,
// dispatches relayed node frames and the 0x60 uplink block, and provides the
// downlink send path. See MainController-Server-Link-Spec.md §3-§6.
package uplink

import (
	"context"
	"errors"
	"log/slog"
	"net"
	"sync"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// Handler receives dispatched events from the active connection. All methods
// are called from the connection's read goroutine, one at a time.
type Handler interface {
	// OnConnect fires after a valid UplinkHello. Use it to request a roster
	// and re-assert overrides.
	OnConnect(hello nodelib.UplinkHello)
	OnDisconnect()

	// OnNodeFrame is a relayed frame from the bus (endpoint 0x10-0x5F).
	OnNodeFrame(f nodelib.Frame)
	OnRosterEntry(e nodelib.RosterEntry)
	OnPresence(p nodelib.NodePresence)
	OnMainStatus(s nodelib.MainStatus)
	OnOtaReport(r nodelib.OtaControlReport)
}

// Server accepts one MainController connection at a time.
type Server struct {
	token [16]byte
	log   *slog.Logger
	h     Handler

	mu      sync.Mutex
	active  *conn
	onState func(up bool)
}

// New builds a server. token is the 16-byte shared secret from config;
// onState is notified on every up/down transition.
func New(token [16]byte, h Handler, onState func(up bool), log *slog.Logger) *Server {
	return &Server{token: token, h: h, onState: onState, log: log}
}

// ListenAndServe blocks serving on addr until ctx is cancelled.
func (s *Server) ListenAndServe(ctx context.Context, addr string) error {
	lc := net.ListenConfig{}
	ln, err := lc.Listen(ctx, "tcp", addr)
	if err != nil {
		return err
	}
	s.log.Info("uplink listening", "addr", ln.Addr().String())
	go func() {
		<-ctx.Done()
		ln.Close()
	}()
	for {
		nc, err := ln.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			s.log.Warn("uplink accept", "err", err)
			continue
		}
		go s.serveConn(ctx, nc)
	}
}

// Connected reports whether a MainController is currently attached.
func (s *Server) Connected() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.active != nil
}

// Send queues a raw frame for downlink. It returns false if no MainController
// is connected or the outbound queue is full (the caller should surface that).
func (s *Server) Send(f nodelib.Frame) bool {
	s.mu.Lock()
	c := s.active
	s.mu.Unlock()
	if c == nil {
		return false
	}
	return c.send(f)
}

// SendGet queues a Get for a node endpoint.
func (s *Server) SendGet(node int, ep nodelib.Endpoint) bool {
	return s.Send(nodelib.Frame{Node: uint8(node), Endpoint: ep, Operation: nodelib.OpGet})
}

// SendSet queues a Set with a raw payload.
func (s *Server) SendSet(node int, ep nodelib.Endpoint, data []byte) bool {
	return s.Send(nodelib.Frame{Node: uint8(node), Endpoint: ep, Operation: nodelib.OpSet, Data: data})
}

func (s *Server) serveConn(ctx context.Context, nc net.Conn) {
	c := newConn(nc, s.log)
	defer nc.Close()

	// First frame must be a valid UplinkHello.
	hello, err := c.readHello(s.token)
	if err != nil {
		s.log.Warn("uplink handshake rejected", "remote", nc.RemoteAddr().String(), "err", err)
		return
	}
	s.log.Info("uplink connected", "remote", nc.RemoteAddr().String(),
		"fw", hello.FWVersion, "nodes", hello.NodeCount)

	s.mu.Lock()
	if s.active != nil {
		s.active.close()
	}
	s.active = c
	s.mu.Unlock()
	if s.onState != nil {
		s.onState(true)
	}
	s.h.OnConnect(hello)

	go c.writeLoop()
	c.readLoop(s.h)

	s.mu.Lock()
	if s.active == c {
		s.active = nil
	}
	s.mu.Unlock()
	c.close()
	if s.onState != nil {
		s.onState(false)
	}
	s.h.OnDisconnect()
	s.log.Info("uplink disconnected", "remote", nc.RemoteAddr().String())
}

// --- per-connection ---------------------------------------------------------

const outboundQueue = 32 // MainController-Server-Link-Spec.md §7.2 backstop

type conn struct {
	nc   net.Conn
	log  *slog.Logger
	df   *nodelib.Deframer
	out  chan []byte
	done chan struct{}
	once sync.Once
}

func newConn(nc net.Conn, log *slog.Logger) *conn {
	return &conn{
		nc:   nc,
		log:  log,
		df:   nodelib.NewDeframer(),
		out:  make(chan []byte, outboundQueue),
		done: make(chan struct{}),
	}
}

func (c *conn) close() {
	c.once.Do(func() {
		close(c.done)
		c.nc.Close()
	})
}

func (c *conn) readHello(token [16]byte) (nodelib.UplinkHello, error) {
	_ = c.nc.SetReadDeadline(time.Now().Add(10 * time.Second))
	buf := make([]byte, 512)
	for {
		n, err := c.nc.Read(buf)
		if err != nil {
			return nodelib.UplinkHello{}, err
		}
		for _, f := range c.df.Push(buf[:n]) {
			if f.Endpoint != nodelib.EndpointUplinkHello || f.Operation != nodelib.OpReport {
				return nodelib.UplinkHello{}, errors.New("first frame is not UplinkHello Report")
			}
			hello, ok := nodelib.ParseUplinkHello(f.Data)
			if !ok {
				return nodelib.UplinkHello{}, errors.New("short UplinkHello")
			}
			if hello.AuthToken != token {
				return nodelib.UplinkHello{}, errors.New("auth token mismatch")
			}
			_ = c.nc.SetReadDeadline(time.Time{})
			return hello, nil
		}
	}
}

func (c *conn) readLoop(h Handler) {
	buf := make([]byte, 4096)
	for {
		_ = c.nc.SetReadDeadline(time.Now().Add(90 * time.Second)) // > Keepalive interval
		n, err := c.nc.Read(buf)
		if err != nil {
			return
		}
		for _, f := range c.df.Push(buf[:n]) {
			c.dispatch(f, h)
		}
	}
}

func (c *conn) dispatch(f nodelib.Frame, h Handler) {
	switch {
	case f.Endpoint.Block() >= 0x10 && f.Endpoint.Block() <= 0x50:
		h.OnNodeFrame(f)

	case f.Endpoint == nodelib.EndpointRoster && f.Operation == nodelib.OpReport:
		if e, ok := nodelib.ParseRosterEntry(f.Data); ok && e.NodeID != nodelib.NodeBroadcast {
			h.OnRosterEntry(e)
		}
	case f.Endpoint == nodelib.EndpointNodePresence && f.Operation == nodelib.OpReport:
		if p, ok := nodelib.ParseNodePresence(f.Data); ok {
			h.OnPresence(p)
		}
	case f.Endpoint == nodelib.EndpointMainStatus && f.Operation == nodelib.OpReport:
		if s, ok := nodelib.ParseMainStatus(f.Data); ok {
			h.OnMainStatus(s)
		}
	case f.Endpoint == nodelib.EndpointOtaControl && f.Operation == nodelib.OpReport:
		if r, ok := nodelib.ParseOtaControlReport(f.Data); ok {
			h.OnOtaReport(r)
		}
	case f.Endpoint == nodelib.EndpointKeepalive:
		c.send(nodelib.Frame{Node: nodelib.NodeMaster, Endpoint: nodelib.EndpointKeepalive, Operation: nodelib.OpReport})
	case f.Endpoint == nodelib.EndpointUplinkHello:
		// re-hello mid-stream: ignore (handshake already validated)
	default:
		c.log.Debug("uplink: unhandled frame", "endpoint", f.Endpoint, "op", f.Operation)
	}
}

func (c *conn) send(f nodelib.Frame) bool {
	raw, err := nodelib.Encode(f)
	if err != nil {
		return false
	}
	select {
	case c.out <- raw:
		return true
	default:
		return false // queue full: MainStatus.downlinkDrops equivalent
	}
}

func (c *conn) writeLoop() {
	for {
		select {
		case <-c.done:
			return
		case raw := <-c.out:
			_ = c.nc.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if _, err := c.nc.Write(raw); err != nil {
				c.close()
				return
			}
		}
	}
}
