package httpapi

import (
	"encoding/json"
	"errors"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	"github.com/jweij/climatecontrol/webserver/internal/service"
	"github.com/jweij/climatecontrol/webserver/internal/store"
)

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, map[string]string{"error": msg})
}

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"ok":       true,
		"uplinkUp": s.svc.MasterOnline(),
	})
}

func (s *Server) handleState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write(s.svc.Hub().SnapshotJSON())
}

func (s *Server) handleNodes(w http.ResponseWriter, r *http.Request) {
	nodes, err := s.svc.Store().Roster(r.Context(), s.svc.MasterOnline())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, nodes)
}

// --- expected-node roster --------------------------------------------

func (s *Server) handleListExpectedNodes(w http.ResponseWriter, r *http.Request) {
	nodes, err := s.svc.Store().ExpectedNodes(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, nodes)
}

type expectedNodeReq struct {
	Module string `json:"module"`
	Name   string `json:"name"`
	Note   string `json:"note"`
}

func (s *Server) handleSetExpectedNode(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(r.PathValue("id"))
	if id <= 0 || id >= nodelib.NodeBroadcast {
		writeErr(w, http.StatusBadRequest, "node id must be 1..254")
		return
	}
	var req expectedNodeReq
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	mod := nodelib.ModuleUnknown
	if req.Module != "" {
		m, ok := nodelib.ModuleByName(req.Module)
		if !ok {
			writeErr(w, http.StatusBadRequest, "unknown module name")
			return
		}
		mod = m
	}
	if err := s.svc.Store().SetExpectedNode(r.Context(), id, mod, req.Name, req.Note, "ui"); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"id": id, "module": mod.String(), "name": req.Name, "note": req.Note, "source": "ui"})
}

func (s *Server) handleDeleteExpectedNode(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(r.PathValue("id"))
	if id <= 0 {
		writeErr(w, http.StatusBadRequest, "bad node id")
		return
	}
	nodes, err := s.svc.Store().ExpectedNodes(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	for _, n := range nodes {
		if n.ID == id && n.Source == "config" {
			writeErr(w, http.StatusConflict, "this node comes from config.json — remove it there and restart")
			return
		}
	}
	if err := s.svc.Store().DeleteExpectedNode(r.Context(), id); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleReadings(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	node, _ := strconv.Atoi(q.Get("node"))
	ep, ok := nodelib.EndpointByName(q.Get("endpoint"))
	if node <= 0 || !ok {
		writeErr(w, http.StatusBadRequest, "node and a valid endpoint name are required")
		return
	}
	to := parseMillis(q.Get("to"), time.Now().UnixMilli())
	from := parseMillis(q.Get("from"), to-24*3600*1000)
	limit, _ := strconv.Atoi(q.Get("limit"))

	points, err := s.svc.Store().Readings(r.Context(), node, ep, from, to, limit)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"node": node, "endpoint": ep.String(), "from": from, "to": to, "points": points,
	})
}

type commandReq struct {
	Node     int     `json:"node"`
	Endpoint string  `json:"endpoint"`
	Value    float64 `json:"value"`
}

func (s *Server) handleCommand(w http.ResponseWriter, r *http.Request) {
	var req commandReq
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	ep, ok := nodelib.EndpointByName(req.Endpoint)
	if req.Node <= 0 || !ok {
		writeErr(w, http.StatusBadRequest, "node and a valid endpoint name are required")
		return
	}
	err := s.svc.SendCommand(r.Context(), req.Node, ep, req.Value, userOf(r))
	switch {
	case errors.Is(err, service.ErrNotWritable):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, service.ErrDownlinkUnavailable):
		writeErr(w, http.StatusServiceUnavailable, err.Error())
	case err != nil:
		writeErr(w, http.StatusInternalServerError, err.Error())
	default:
		writeJSON(w, http.StatusAccepted, map[string]string{"status": "queued"})
	}
}

func (s *Server) handleListOverrides(w http.ResponseWriter, r *http.Request) {
	node, _ := strconv.Atoi(r.URL.Query().Get("node"))
	ovs, err := s.svc.Store().Overrides(r.Context(), node)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, ovs)
}

func (s *Server) handleDeleteOverride(w http.ResponseWriter, r *http.Request) {
	node, _ := strconv.Atoi(r.PathValue("node"))
	ep, ok := nodelib.EndpointByName(r.PathValue("endpoint"))
	if node <= 0 || !ok {
		writeErr(w, http.StatusBadRequest, "bad node/endpoint")
		return
	}
	if err := s.svc.Store().DeleteOverride(r.Context(), node, ep); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// --- map ---------------------------------------------------------------

func (s *Server) handleListFloors(w http.ResponseWriter, r *http.Request) {
	floors, err := s.svc.Store().Floors(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, floors)
}

func (s *Server) handleUploadFloor(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(32 << 20); err != nil {
		writeErr(w, http.StatusBadRequest, "expected multipart form with 'image' and 'name'")
		return
	}
	name := strings.TrimSpace(r.FormValue("name"))
	if name == "" {
		name = "Floor"
	}
	file, hdr, err := r.FormFile("image")
	if err != nil {
		writeErr(w, http.StatusBadRequest, "missing 'image' file")
		return
	}
	defer file.Close()

	data, err := io.ReadAll(io.LimitReader(file, 32<<20))
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	cfg, _, err := image.DecodeConfig(strings.NewReader(string(data)))
	if err != nil {
		writeErr(w, http.StatusBadRequest, "not a decodable PNG/JPEG/GIF image")
		return
	}

	if err := os.MkdirAll(s.assetDir, 0o755); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	fname := "floor-" + strconv.FormatInt(time.Now().UnixNano(), 36) + filepath.Ext(hdr.Filename)
	path := filepath.Join(s.assetDir, fname)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	id, err := s.svc.Store().AddFloor(r.Context(), name, path, cfg.Width, cfg.Height)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, store.Floor{
		ID: id, Name: name, WidthPx: cfg.Width, HeightPx: cfg.Height,
	})
}

func (s *Server) handleDeleteFloor(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(r.PathValue("id"))
	if err := s.svc.Store().DeleteFloor(r.Context(), id); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Server) handleFloorImage(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.Atoi(r.PathValue("id"))
	f, err := s.svc.Store().Floor(r.Context(), id)
	if err != nil {
		writeErr(w, http.StatusNotFound, "no such floor")
		return
	}
	http.ServeFile(w, r, f.ImagePath)
}

func (s *Server) handleListPlacements(w http.ResponseWriter, r *http.Request) {
	ps, err := s.svc.Store().Placements(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, ps)
}

func (s *Server) handleSetPlacement(w http.ResponseWriter, r *http.Request) {
	node, _ := strconv.Atoi(r.PathValue("node"))
	if node <= 0 {
		writeErr(w, http.StatusBadRequest, "bad node")
		return
	}
	var p store.Placement
	if err := json.NewDecoder(io.LimitReader(r.Body, 1<<20)).Decode(&p); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	p.NodeID = node
	if err := s.svc.Store().SetPlacement(r.Context(), p); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, p)
}

func (s *Server) handleDeletePlacement(w http.ResponseWriter, r *http.Request) {
	node, _ := strconv.Atoi(r.PathValue("node"))
	if err := s.svc.Store().DeletePlacement(r.Context(), node); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// --- OTA ---------------------------------------------------------------

func (s *Server) handleStartOTA(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(2 << 20); err != nil {
		writeErr(w, http.StatusBadRequest, "expected multipart form with 'image' and 'node'")
		return
	}
	node, _ := strconv.Atoi(r.FormValue("node"))
	if node <= 0 {
		writeErr(w, http.StatusBadRequest, "missing 'node'")
		return
	}
	target := r.FormValue("target")
	if target == "" {
		target = "node"
	}
	file, hdr, err := r.FormFile("image")
	if err != nil {
		writeErr(w, http.StatusBadRequest, "missing 'image' file")
		return
	}
	defer file.Close()
	bin, err := io.ReadAll(io.LimitReader(file, 1<<20))
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}

	jobID, err := s.svc.StartOTA(r.Context(), node, target, hdr.Filename, bin, filepath.Join(s.assetDir, "ota"))
	switch {
	case errors.Is(err, service.ErrOtaBusy), errors.Is(err, service.ErrOtaQueued):
		writeErr(w, http.StatusConflict, err.Error())
	case errors.Is(err, service.ErrOtaTargetMismatch):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, service.ErrDownlinkUnavailable):
		writeErr(w, http.StatusServiceUnavailable, err.Error())
	case errors.Is(err, nodelib.ErrBadImage):
		writeErr(w, http.StatusBadRequest, "not a valid ClimateControl image (bad magic/CRC)")
	case err != nil:
		writeErr(w, http.StatusInternalServerError, err.Error())
	default:
		writeJSON(w, http.StatusAccepted, map[string]any{"jobId": jobID})
	}
}

// --- firmware repository ---------------------------------------------

func (s *Server) firmwareRepoDir() string { return filepath.Join(s.assetDir, "firmware") }
func (s *Server) otaJobDir() string       { return filepath.Join(s.assetDir, "ota") }

func (s *Server) handleFirmwareView(w http.ResponseWriter, r *http.Request) {
	view, err := s.svc.FirmwareView(r.Context())
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, view)
}

func (s *Server) handleUploadFirmware(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseMultipartForm(2 << 20); err != nil {
		writeErr(w, http.StatusBadRequest, "expected multipart form with 'image'")
		return
	}
	file, hdr, err := r.FormFile("image")
	if err != nil {
		writeErr(w, http.StatusBadRequest, "missing 'image' file")
		return
	}
	defer file.Close()
	bin, err := io.ReadAll(io.LimitReader(file, 1<<20))
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}

	fi, err := s.svc.StoreFirmware(r.Context(), hdr.Filename, bin, s.firmwareRepoDir())
	switch {
	case errors.Is(err, service.ErrBadFirmwareName), errors.Is(err, service.ErrFirmwareDescMismatch):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, nodelib.ErrBadImage):
		writeErr(w, http.StatusBadRequest, "not a valid ClimateControl image (bad magic/CRC)")
	case err != nil:
		writeErr(w, http.StatusInternalServerError, err.Error())
	default:
		writeJSON(w, http.StatusCreated, fi)
	}
}

func (s *Server) handleDeleteFirmware(w http.ResponseWriter, r *http.Request) {
	mod, ok := nodelib.ModuleByName(r.PathValue("module"))
	if !ok || mod == nodelib.ModuleUnknown {
		writeErr(w, http.StatusBadRequest, "unknown module")
		return
	}
	if err := s.svc.DeleteFirmware(r.Context(), mod); err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

type fwUpdateReq struct {
	Node   *int   `json:"node"` // 0 = the MainController itself (self-update)
	Target string `json:"target"`
}

func (s *Server) handleFirmwareUpdate(w http.ResponseWriter, r *http.Request) {
	var req fwUpdateReq
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	if req.Target == "" {
		req.Target = "node"
	}
	if req.Node == nil || *req.Node < 0 || *req.Node >= nodelib.NodeBroadcast {
		writeErr(w, http.StatusBadRequest, "missing or out-of-range 'node' (0..254; 0 = MainController)")
		return
	}
	if *req.Node == 0 && req.Target != "node" {
		writeErr(w, http.StatusBadRequest, "the MainController has no paired thermostat")
		return
	}
	jobID, err := s.svc.EnqueueUpdate(r.Context(), *req.Node, req.Target, s.otaJobDir())
	s.writeEnqueueResult(w, []int64{jobID}, err)
}

type fwUpdateAllReq struct {
	Module string `json:"module"`
}

func (s *Server) handleFirmwareUpdateAll(w http.ResponseWriter, r *http.Request) {
	var req fwUpdateAllReq
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON")
		return
	}
	if _, ok := nodelib.ModuleByName(req.Module); !ok {
		writeErr(w, http.StatusBadRequest, "unknown module")
		return
	}
	ids, err := s.svc.EnqueueUpdateAll(r.Context(), req.Module, s.otaJobDir())
	s.writeEnqueueResult(w, ids, err)
}

func (s *Server) writeEnqueueResult(w http.ResponseWriter, ids []int64, err error) {
	switch {
	case errors.Is(err, service.ErrOtaQueued):
		writeErr(w, http.StatusConflict, err.Error())
	case errors.Is(err, service.ErrNoFirmwareImage):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, service.ErrUnknownNodeModule):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, service.ErrOtaTargetMismatch):
		writeErr(w, http.StatusBadRequest, err.Error())
	case errors.Is(err, service.ErrDownlinkUnavailable):
		writeErr(w, http.StatusServiceUnavailable, err.Error())
	case err != nil:
		writeErr(w, http.StatusInternalServerError, err.Error())
	default:
		queued := 0
		for _, id := range ids {
			if id > 0 {
				queued++
			}
		}
		writeJSON(w, http.StatusAccepted, map[string]any{"queued": queued, "jobIds": ids})
	}
}

func (s *Server) handleListOTA(w http.ResponseWriter, r *http.Request) {
	jobs, err := s.svc.Store().OtaJobs(r.Context(), 50)
	if err != nil {
		writeErr(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, jobs)
}

func (s *Server) handleGetOTA(w http.ResponseWriter, r *http.Request) {
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64)
	job, err := s.svc.Store().OtaJob(r.Context(), id)
	if err != nil {
		writeErr(w, http.StatusNotFound, "no such job")
		return
	}
	writeJSON(w, http.StatusOK, job)
}

func parseMillis(s string, def int64) int64 {
	if s == "" {
		return def
	}
	if v, err := strconv.ParseInt(s, 10, 64); err == nil {
		return v
	}
	if t, err := time.Parse(time.RFC3339, s); err == nil {
		return t.UnixMilli()
	}
	return def
}

func userOf(r *http.Request) string {
	if u := r.Header.Get("X-Forwarded-User"); u != "" {
		return u
	}
	return "operator"
}
