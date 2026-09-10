package httpapi

import (
	"net/http"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/ws"
)

// handleWS upgrades to a WebSocket, sends the current-state snapshot, then
// streams every hub event for the life of the connection.
func (s *Server) handleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := ws.Accept(w, r)
	if err != nil {
		return // Accept already wrote the error
	}
	defer conn.Close()

	events, unsubscribe := s.svc.Hub().Subscribe()
	defer unsubscribe()

	if err := conn.WriteText(s.svc.Hub().SnapshotJSON()); err != nil {
		return
	}

	// Reader goroutine: handles client pings and detects disconnect.
	readErr := make(chan error, 1)
	go func() { readErr <- conn.ReadLoop() }()

	ping := time.NewTicker(30 * time.Second)
	defer ping.Stop()

	for {
		select {
		case <-readErr:
			return
		case <-ping.C:
			if err := conn.WriteText([]byte(`{"type":"ping"}`)); err != nil {
				return
			}
		case msg, ok := <-events:
			if !ok {
				return
			}
			if err := conn.WriteText(msg); err != nil {
				return
			}
		}
	}
}
