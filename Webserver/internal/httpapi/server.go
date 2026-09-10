// Package httpapi serves the three surfaces on one port: the embedded SPA
// bundle, the JSON API under /api, and the live WebSocket at /ws.
// See MainController-Server-Link-Spec.md §6.
package httpapi

import (
	"io/fs"
	"log/slog"
	"net/http"

	"github.com/jweij/climatecontrol/webserver/internal/service"
)

// Server holds the HTTP dependencies.
type Server struct {
	svc      *service.Service
	log      *slog.Logger
	assetDir string
	webFS    fs.FS
}

// New builds the HTTP server. webFS is the embedded UI (rooted at the files,
// i.e. contains index.html at its root).
func New(svc *service.Service, webFS fs.FS, assetDir string, log *slog.Logger) *Server {
	return &Server{svc: svc, log: log, assetDir: assetDir, webFS: webFS}
}

// Handler returns the root mux.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()

	// Live channel.
	mux.HandleFunc("GET /ws", s.handleWS)

	// JSON API.
	mux.HandleFunc("GET /api/health", s.handleHealth)
	mux.HandleFunc("GET /api/state", s.handleState)
	mux.HandleFunc("GET /api/nodes", s.handleNodes)
	mux.HandleFunc("GET /api/expected-nodes", s.handleListExpectedNodes)
	mux.HandleFunc("PUT /api/expected-nodes/{id}", s.handleSetExpectedNode)
	mux.HandleFunc("DELETE /api/expected-nodes/{id}", s.handleDeleteExpectedNode)
	mux.HandleFunc("GET /api/readings", s.handleReadings)
	mux.HandleFunc("POST /api/commands", s.handleCommand)
	mux.HandleFunc("GET /api/overrides", s.handleListOverrides)
	mux.HandleFunc("DELETE /api/overrides/{node}/{endpoint}", s.handleDeleteOverride)

	mux.HandleFunc("GET /api/floors", s.handleListFloors)
	mux.HandleFunc("POST /api/floors", s.handleUploadFloor)
	mux.HandleFunc("DELETE /api/floors/{id}", s.handleDeleteFloor)
	mux.HandleFunc("GET /api/floors/{id}/image", s.handleFloorImage)
	mux.HandleFunc("GET /api/placements", s.handleListPlacements)
	mux.HandleFunc("PUT /api/placements/{node}", s.handleSetPlacement)
	mux.HandleFunc("DELETE /api/placements/{node}", s.handleDeletePlacement)

	mux.HandleFunc("POST /api/ota", s.handleStartOTA)
	mux.HandleFunc("GET /api/ota", s.handleListOTA)
	mux.HandleFunc("GET /api/ota/{id}", s.handleGetOTA)

	mux.HandleFunc("GET /api/firmware", s.handleFirmwareView)
	mux.HandleFunc("POST /api/firmware", s.handleUploadFirmware)
	mux.HandleFunc("DELETE /api/firmware/{module}", s.handleDeleteFirmware)
	mux.HandleFunc("POST /api/firmware/update", s.handleFirmwareUpdate)
	mux.HandleFunc("POST /api/firmware/update-all", s.handleFirmwareUpdateAll)

	// SPA bundle: static assets, everything else falls through to index.html.
	mux.Handle("GET /", s.spaHandler())

	return logging(s.log, mux)
}

func (s *Server) spaHandler() http.Handler {
	fileServer := http.FileServer(http.FS(s.webFS))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, err := fs.Stat(s.webFS, trimLeadingSlash(r.URL.Path)); err == nil {
			fileServer.ServeHTTP(w, r)
			return
		}
		// Unknown path: serve the app shell so client-side routes work.
		r2 := r.Clone(r.Context())
		r2.URL.Path = "/"
		fileServer.ServeHTTP(w, r2)
	})
}

func trimLeadingSlash(p string) string {
	if p == "/" {
		return "index.html"
	}
	if len(p) > 0 && p[0] == '/' {
		return p[1:]
	}
	return p
}

func logging(log *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		next.ServeHTTP(w, r)
		if r.URL.Path != "/ws" {
			log.Debug("http", "method", r.Method, "path", r.URL.Path)
		}
	})
}
