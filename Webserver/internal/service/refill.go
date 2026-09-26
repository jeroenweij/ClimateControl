package service

import (
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

// Nodes report state on change only, so after this server restarts (its
// live-value cache is memory-only) or a node rejoins, a value that isn't
// changing would stay blank indefinitely. The state endpoints per module are
// asked for explicitly instead (MainController-Server-Link-Spec.md, "Uplink
// down" row: re-pull state after reconnect).
var stateEndpoints = map[nodelib.Module][]nodelib.Endpoint{
	nodelib.ModuleControllerNode: {
		nodelib.EndpointDamperTarget, nodelib.EndpointDamperActual, nodelib.EndpointDamperMode,
		nodelib.EndpointRoomTemp, nodelib.EndpointRoomSetpoint, nodelib.EndpointSystemStatus,
	},
	nodelib.ModuleTemperatureNode: {
		nodelib.EndpointSupplyTemp, nodelib.EndpointReturnTemp, nodelib.EndpointSystemStatus,
	},
}

// Spacing between refill Gets: the MainController relays each onto the bus
// through a 25-deep queue that drains once per poll cycle, so a burst for
// every node at once could overflow it.
const refillGapDefault = 25 * time.Millisecond

type refillReq struct {
	node int
	eps  []nodelib.Endpoint
}

// refillState queues Gets for a node's state endpoints -- all of them, or
// with onlyMissing just those the live cache has no value for. A single
// worker sends them, paced, so the uplink reader is never held up.
func (s *Service) refillState(node int, m nodelib.Module, onlyMissing bool) {
	var eps []nodelib.Endpoint
	for _, ep := range stateEndpoints[m] {
		if onlyMissing {
			if _, ok := s.hb.CurrentValue(node, ep); ok {
				continue
			}
		}
		eps = append(eps, ep)
	}
	if len(eps) == 0 {
		return
	}
	s.refillOnce.Do(func() { go s.refillWorker() })
	select {
	case s.refillQ <- refillReq{node: node, eps: eps}:
	default:
		s.log.Warn("state refill queue full, dropping", "node", node)
	}
}

func (s *Service) refillWorker() {
	for req := range s.refillQ {
		for _, ep := range req.eps {
			s.send.SendGet(req.node, ep)
			time.Sleep(s.refillGap)
		}
	}
}
