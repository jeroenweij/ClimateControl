package service

import (
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// NodeFaults is one node's fault picture for the status page: what is wrong
// right now, and the most recent fault raised -- kept after it clears, so a
// transient one (a damper stall that the next move clears) is still visible.
type NodeFaults struct {
	Active []string `json:"faults"`
	Last   string   `json:"lastFault,omitempty"`
	LastTS int64    `json:"lastFaultTs,omitempty"` // unix millis when Last was raised
	flags  uint16
}

func (s *Service) setModule(node int, m nodelib.Module) {
	if m == nodelib.ModuleUnknown {
		return
	}
	s.mu.Lock()
	s.modules[node] = m
	s.mu.Unlock()
}

// onStatus tracks a node's SystemStatus errorFlags -- pushed by the node
// whenever they change (Node::ReportStatusIfChanged) -- and tells the UI when
// the picture changed.
func (s *Service) onStatus(node int, v nodelib.Value) {
	flags, ok := v.Fields["errorFlags"].(uint16)
	if !ok {
		return
	}
	s.mu.Lock()
	f := s.faults[node]
	if f == nil {
		f = &NodeFaults{}
		s.faults[node] = f
	}
	changed := flags != f.flags || f.Active == nil
	if raised := flags &^ f.flags; raised != 0 {
		names := nodelib.FaultNames(s.modules[node], raised)
		f.Last = names[len(names)-1]
		f.LastTS = time.Now().UnixMilli()
	}
	f.flags = flags
	f.Active = nodelib.FaultNames(s.modules[node], flags)
	if f.Active == nil {
		f.Active = []string{} // "known, none" -- distinct from never reported
	}
	s.mu.Unlock()
	if changed {
		s.hb.PublishFault(node)
	}
}

// Faults returns a copy of a node's fault picture (zero value if it never
// reported a SystemStatus).
func (s *Service) Faults(node int) NodeFaults {
	s.mu.Lock()
	defer s.mu.Unlock()
	f := s.faults[node]
	if f == nil {
		return NodeFaults{}
	}
	out := *f
	out.Active = append([]string(nil), f.Active...)
	return out
}
