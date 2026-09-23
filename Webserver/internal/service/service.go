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
	mcFW int // MainController running firmware (major<<8|minor), from UplinkHello; 0 = unknown

	// Nodes we've already asked for SystemInfo this process's lifetime, so a
	// node's steady stream of ordinary Reports doesn't re-send the Get on
	// every single one while we wait for the (once-per-boot) answer.
	fwRequested map[int]bool

	// Operator toggle (default off, not persisted -- resets to off on every
	// restart): lets EnqueueUpdate/EnqueueUpdateAll re-push a same-or-older
	// version image, for bench-testing the OTA path itself without bumping
	// CC_FW_VERSION on every build.
	allowDowngrade bool
}

// New builds the service. Call SetSender once the uplink server exists.
func New(st *store.Store, hb *hub.Hub, log *slog.Logger) *Service {
	return &Service{st: st, hb: hb, log: log, fwRequested: make(map[int]bool)}
}

// SetSender installs the downlink path (breaks the construction cycle).
func (s *Service) SetSender(snd Sender) { s.send = snd }

// Store and Hub expose the dependencies to the HTTP layer.
func (s *Service) Store() *store.Store { return s.st }
func (s *Service) Hub() *hub.Hub       { return s.hb }

// MasterOnline reports whether a MainController is currently connected. When it
// is not, every node is treated as offline (there is no bus to hear them on).
func (s *Service) MasterOnline() bool {
	return s.send != nil && s.send.Connected()
}

// MainControllerFW returns the MainController's running firmware version
// (major<<8|minor) as reported in its last UplinkHello, or 0 if it has never
// connected.
func (s *Service) MainControllerFW() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.mcFW
}

// AllowDowngrade reports whether same/older-version firmware pushes are
// currently permitted (see the allowDowngrade field comment).
func (s *Service) AllowDowngrade() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.allowDowngrade
}

// SetAllowDowngrade flips the toggle.
func (s *Service) SetAllowDowngrade(v bool) {
	s.mu.Lock()
	s.allowDowngrade = v
	s.mu.Unlock()
}

// warnIfUnexpected logs a node id that showed up on the bus without an entry in
// the expected roster.
func (s *Service) warnIfUnexpected(id int, module nodelib.Module) {
	exp, err := s.st.ExpectedNodes(context.Background())
	if err != nil {
		return
	}
	for _, e := range exp {
		if e.ID == id {
			return
		}
	}
	s.log.Warn("unexpected node on bus (not in expected roster)", "node", id, "module", module.String())
}

// --- uplink.Handler --------------------------------------------------------

// OnConnect asks for a fresh roster and re-asserts stored overrides.
func (s *Service) OnConnect(h nodelib.UplinkHello) {
	s.hb.SetUplink(true)
	s.mu.Lock()
	s.mcFW = int(h.FWVersion)
	s.mu.Unlock()
	s.send.Send(nodelib.Frame{Node: nodelib.NodeMaster, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpGet})
	s.reassertOverrides(0)
	s.kickOta() // resume any push that was waiting for the downlink
}

// OnDisconnect flags the uplink down.
func (s *Service) OnDisconnect() { s.hb.SetUplink(false) }

// OnNodeFrame handles a relayed bus frame.
func (s *Service) OnNodeFrame(f nodelib.Frame) {
	ctx := context.Background()
	node := int(f.Node)

	switch f.Operation {
	case nodelib.OpReport:
		if f.Endpoint == nodelib.EndpointFirmware || f.Endpoint == nodelib.EndpointThermostatFirmware {
			// OTA progress, not sensor data -- hand it to the driver instead
			// of the readings store (ota.go).
			s.onFirmwareReport(f)
			return
		}
		v := nodelib.DecodeValue(f.Endpoint, f.Data)
		now := time.Now().UnixMilli()
		if err := s.st.InsertReading(ctx, now, node, f.Endpoint, f.Data, v); err != nil {
			s.log.Warn("store reading", "err", err)
		}
		_ = s.st.UpsertNode(ctx, node, moduleFromInfo(f, v), true)
		if fw := firmwareFromInfo(f, v); fw != 0 {
			_ = s.st.SetNodeFirmware(ctx, node, fw)
		} else if f.Endpoint != nodelib.EndpointSystemInfo {
			s.mu.Lock()
			asked := s.fwRequested[node]
			s.fwRequested[node] = true
			s.mu.Unlock()
			if !asked {
				s.send.SendGet(node, nodelib.EndpointSystemInfo)
			}
		}
		s.hb.PublishValue(node, f.Endpoint, v)

	case nodelib.OpAck:
		if f.Endpoint == nodelib.EndpointFirmware || f.Endpoint == nodelib.EndpointThermostatFirmware {
			s.onFirmwareWriteReply(false, f)
			return
		}
		s.log.Debug("node ack", "node", node, "endpoint", f.Endpoint)
	case nodelib.OpNack:
		if f.Endpoint == nodelib.EndpointFirmware || f.Endpoint == nodelib.EndpointThermostatFirmware {
			s.onFirmwareWriteReply(true, f)
			return
		}
		s.log.Info("node nack", "node", node, "endpoint", f.Endpoint, "reason", f.Data)
	}
}

// OnRosterEntry records one node from a roster stream.
func (s *Service) OnRosterEntry(e nodelib.RosterEntry) {
	ctx := context.Background()
	_ = s.st.UpsertNode(ctx, int(e.NodeID), e.Module, true)
	_ = s.st.SetNodeState(ctx, int(e.NodeID), int(e.State))
	s.warnIfUnexpected(int(e.NodeID), e.Module)
	s.hb.PublishPresence(int(e.NodeID), e.Module, true)
	// SystemInfo (running firmware version) is Get-only on the node side --
	// it's never self-reported (Node.cpp's HandleSystemMessage), so the
	// Firmware tab's "Installed" column stays unknown unless something asks.
	s.send.SendGet(int(e.NodeID), nodelib.EndpointSystemInfo)
}

// OnPresence records a node up/down transition. Uses UpsertNode (an upsert)
// rather than a plain UPDATE so a node whose very first sighting is a live
// NodePresence (up between two Roster dumps, not yet in the table at all)
// still gets a row -- a plain UPDATE would silently affect nothing.
func (s *Service) OnPresence(p nodelib.NodePresence) {
	ctx := context.Background()
	_ = s.st.UpsertNode(ctx, int(p.NodeID), p.Module, p.Up)
	state := 0
	if p.Bootloader {
		state = 1
	}
	_ = s.st.SetNodeState(ctx, int(p.NodeID), state)
	s.hb.PublishPresence(int(p.NodeID), p.Module, p.Up)
	if p.Up {
		s.warnIfUnexpected(int(p.NodeID), p.Module)
		s.reassertOverrides(int(p.NodeID))
		s.send.SendGet(int(p.NodeID), nodelib.EndpointSystemInfo)
	}
}

// OnMainStatus publishes MainController/bus health.
func (s *Service) OnMainStatus(st nodelib.MainStatus) {
	s.hb.PublishMain(st, true)
}

// OnThermostatStatus records the link state + running firmware of the
// Thermostat paired to one ControllerNode (0x63, spec §5.6).
func (s *Service) OnThermostatStatus(t nodelib.ThermostatStatus) {
	if err := s.st.UpsertThermostat(context.Background(), t); err != nil {
		s.log.Warn("store thermostat status", "err", err)
	}
	s.hb.PublishThermostat(int(t.ControllerNodeID), t.LinkUp)
}

// OnOtaFrame feeds an OtaControl / OtaData frame from MainController's
// bootloader to the active push driver, if it is a MainController push.
func (s *Service) OnOtaFrame(f nodelib.Frame) {
	s.mu.Lock()
	d := s.ota
	s.mu.Unlock()
	if d != nil {
		d.onOtaFrame(f)
	}
}

// onFirmwareReport feeds a relayed Firmware / ThermostatFirmware Report to
// the active push driver, if this frame belongs to it.
func (s *Service) onFirmwareReport(f nodelib.Frame) {
	s.mu.Lock()
	d := s.ota
	s.mu.Unlock()
	if d != nil {
		d.onReport(f)
	}
}

// onFirmwareWriteReply feeds a relayed Firmware / ThermostatFirmware Ack/Nack
// (a Write reply, Node-Flash-Layout-and-Bootloader-Spec.md §6.2.1) to the
// active push driver, if this frame belongs to it.
func (s *Service) onFirmwareWriteReply(nack bool, f nodelib.Frame) {
	s.mu.Lock()
	d := s.ota
	s.mu.Unlock()
	if d != nil {
		d.onWriteReply(nack, f)
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

// firmwareFromInfo pulls the running version (major<<8 | minor) out of a
// decoded SystemInfo payload; 0 when the frame is not SystemInfo.
func firmwareFromInfo(f nodelib.Frame, v nodelib.Value) int {
	if f.Endpoint != nodelib.EndpointSystemInfo || v.Fields == nil {
		return 0
	}
	maj, _ := v.Fields["fwMajor"].(uint16)
	min, _ := v.Fields["fwMinor"].(uint16)
	return int(maj)<<8 | int(min)
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
