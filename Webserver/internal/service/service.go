// Package service wires the uplink connection to the store and the hub: it
// implements uplink.Handler, persists readings, maintains the roster, and
// drives firmware pushes. See MainController-Server-Link-Spec.md §6-§8.
package service

import (
	"context"
	"log/slog"
	"sync"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/hub"
	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	"github.com/jweij/climatecontrol/webserver/internal/store"
	"github.com/jweij/climatecontrol/webserver/internal/uplink"
)

// Sender is the downlink surface the service needs (implemented by
// *uplink.Server).
type Sender interface {
	Send(nodelib.Frame) bool
	SendGet(node int, ep nodelib.Endpoint) bool
	SendSet(node int, ep nodelib.Endpoint, data []byte) bool
	Connected() bool
}

// Service is the application core.
type Service struct {
	st  *store.Store
	hb  *hub.Hub
	log *slog.Logger

	mu   sync.Mutex
	send Sender
	ota  *otaDriver
}

// New builds the service. Call SetSender once the uplink server exists.
func New(st *store.Store, hb *hub.Hub, log *slog.Logger) *Service {
	return &Service{st: st, hb: hb, log: log}
}

// SetSender installs the downlink path (breaks the construction cycle).
func (s *Service) SetSender(snd Sender) { s.send = snd }

// Store and Hub expose the dependencies to the HTTP layer.
func (s *Service) Store() *store.Store { return s.st }
func (s *Service) Hub() *hub.Hub       { return s.hb }

// --- uplink.Handler --------------------------------------------------------

// OnConnect asks for a fresh roster and re-asserts stored overrides.
func (s *Service) OnConnect(h nodelib.UplinkHello) {
	s.hb.SetUplink(true)
	s.send.Send(nodelib.Frame{Node: nodelib.NodeMaster, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpGet})
	s.reassertOverrides(0)
}

// OnDisconnect flags the uplink down.
func (s *Service) OnDisconnect() { s.hb.SetUplink(false) }

// OnNodeFrame handles a relayed bus frame.
func (s *Service) OnNodeFrame(f nodelib.Frame) {
	ctx := context.Background()
	node := int(f.Node)

	switch f.Operation {
	case nodelib.OpReport:
		v := nodelib.DecodeValue(f.Endpoint, f.Data)
		now := time.Now().UnixMilli()
		if err := s.st.InsertReading(ctx, now, node, f.Endpoint, f.Data, v); err != nil {
			s.log.Warn("store reading", "err", err)
		}
		_ = s.st.UpsertNode(ctx, node, moduleFromInfo(f, v), true)
		s.hb.PublishValue(node, f.Endpoint, v)

	case nodelib.OpAck:
		s.log.Debug("node ack", "node", node, "endpoint", f.Endpoint)
	case nodelib.OpNack:
		s.log.Info("node nack", "node", node, "endpoint", f.Endpoint, "reason", f.Data)
	}
}

// OnRosterEntry records one node from a roster stream.
func (s *Service) OnRosterEntry(e nodelib.RosterEntry) {
	_ = s.st.UpsertNode(context.Background(), int(e.NodeID), e.Module, true)
	s.hb.PublishPresence(int(e.NodeID), e.Module, true)
}

// OnPresence records a node up/down transition.
func (s *Service) OnPresence(p nodelib.NodePresence) {
	_ = s.st.SetNodeOnline(context.Background(), int(p.NodeID), p.Up)
	s.hb.PublishPresence(int(p.NodeID), p.Module, p.Up)
	if p.Up {
		s.reassertOverrides(int(p.NodeID))
	}
}

// OnMainStatus publishes MainController/bus health.
func (s *Service) OnMainStatus(st nodelib.MainStatus) {
	s.hb.PublishMain(st, true)
}

// OnOtaReport feeds the firmware-push driver.
func (s *Service) OnOtaReport(r nodelib.OtaControlReport) {
	s.mu.Lock()
	d := s.ota
	s.mu.Unlock()
	if d != nil {
		d.onReport(r)
	}
}

// --- overrides -----------------------------------------------------------

func (s *Service) reassertOverrides(node int) {
	ovs, err := s.st.Overrides(context.Background(), node)
	if err != nil {
		s.log.Warn("load overrides", "err", err)
		return
	}
	for _, o := range ovs {
		ep, ok := nodelib.EndpointByName(o.Endpoint)
		if !ok {
			continue
		}
		data, ok := nodelib.EncodeValue(ep, o.Value)
		if !ok {
			continue
		}
		s.send.SendSet(o.NodeID, ep, data)
	}
}

// SendCommand issues an operator Set, logs it, and (for writable endpoints)
// stores the value as an override.
func (s *Service) SendCommand(ctx context.Context, node int, ep nodelib.Endpoint, value float64, user string) error {
	data, ok := nodelib.EncodeValue(ep, value)
	if !ok {
		return ErrNotWritable
	}
	if _, err := s.st.LogCommand(ctx, node, ep, nodelib.OpSet, data, user); err != nil {
		s.log.Warn("log command", "err", err)
	}
	if !s.send.SendSet(node, ep, data) {
		return ErrDownlinkUnavailable
	}
	if ep != nodelib.EndpointSystemControl {
		_ = s.st.SetOverride(ctx, node, ep, value, user)
	}
	return nil
}

func moduleFromInfo(f nodelib.Frame, v nodelib.Value) nodelib.Module {
	if f.Endpoint != nodelib.EndpointSystemInfo || v.Fields == nil {
		return nodelib.ModuleUnknown
	}
	if name, _ := v.Fields["module"].(string); name != "" {
		for m := nodelib.ModuleUnknown; m <= nodelib.ModuleThermostat; m++ {
			if m.String() == name {
				return m
			}
		}
	}
	return nodelib.ModuleUnknown
}

var _ uplink.Handler = (*Service)(nil)
