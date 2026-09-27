package service

import (
	"context"
	"encoding/binary"
	"time"

	"climatecontrol/webserver/internal/nodelib"
)

// The MainController answers SystemStatus and DiagLastError for itself
// (NODE = 0, MainController-Server-Link-Spec.md §5), the way a bus node does.
// They are asked for once per connection of its application: a reset --
// commanded, watchdog or HardFault -- always reconnects, so that is when
// something new can be said. The replies come in through OnNodeFrame but are
// not a bus node's: they are stored and published under node 0, and kept out
// of the roster.

func (s *Service) requestMainStatus() {
	s.send.SendGet(0, nodelib.EndpointSystemStatus)
	s.send.SendGet(0, nodelib.EndpointDiagLastError)
}

func (s *Service) onMainFrame(f nodelib.Frame) {
	if f.Operation != nodelib.OpReport {
		s.log.Debug("MainController reply", "endpoint", f.Endpoint, "op", f.Operation)
		return
	}
	if f.Endpoint != nodelib.EndpointSystemStatus && f.Endpoint != nodelib.EndpointDiagLastError {
		return
	}
	v := nodelib.DecodeValue(f.Endpoint, f.Data)
	if err := s.st.InsertReading(context.Background(), time.Now().UnixMilli(), 0, f.Endpoint, f.Data, v); err != nil {
		s.log.Warn("store reading", "err", err)
	}
	if f.Endpoint == nodelib.EndpointDiagLastError && len(f.Data) >= 7 && f.Data[0] != nodelib.FaultNone {
		s.log.Warn("MainController was reset by a fault",
			"fault", nodelib.FaultCodeName(f.Data[0]),
			"uptimeMs", binary.LittleEndian.Uint32(f.Data[1:]),
			"context", binary.LittleEndian.Uint16(f.Data[5:]))
	}
	s.hb.PublishValue(0, f.Endpoint, v)
}
