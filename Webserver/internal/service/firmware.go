package service

import (
	"context"
	"os"
	"path/filepath"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
	"github.com/jweij/climatecontrol/webserver/internal/store"
)

// FwTarget is one updatable thing on the Firmware page: a bus node, or the
// Thermostat paired to a ControllerNode (which gets its own row).
type FwTarget struct {
	NodeID       int    `json:"nodeId"`
	Name         string `json:"name"`
	Module       string `json:"module"` // the image module this target needs
	Target       string `json:"target"` // "node" | "thermostat"
	Status       string `json:"status"` // online | offline | unexpected | link-down
	Online       bool   `json:"online"`
	Installed    int    `json:"installed"` // running version, 0 = unknown
	InstalledStr string `json:"installedStr"`
	Latest       int    `json:"latest"` // held image version, 0 = none held
	LatestStr    string `json:"latestStr"`
	CanUpdate    bool   `json:"canUpdate"`
	Reason       string `json:"reason"`        // why CanUpdate is false
	Job          string `json:"job,omitempty"` // state of a queued/running job for this target
}

// FirmwareView is the whole Firmware page payload.
type FirmwareView struct {
	Images  []store.FirmwareImage `json:"images"`
	Targets []FwTarget            `json:"targets"`
}

// StoreFirmware validates an uploaded image and records it as the one image
// held for its module, replacing (and deleting the file of) any previous one.
// The module and version come from the filename
// (nodelib.ParseFirmwareName); the image bytes are checked for the descriptor
// magic / CRC and, when the descriptor names a module or version, those must
// agree with the filename.
func (s *Service) StoreFirmware(ctx context.Context, filename string, bin []byte, repoDir string) (store.FirmwareImage, error) {
	fn, ok := nodelib.ParseFirmwareName(filename)
	if !ok {
		return store.FirmwareImage{}, ErrBadFirmwareName
	}

	desc, crc, err := nodelib.ParseImage(bin)
	if err != nil {
		return store.FirmwareImage{}, err
	}
	if desc.Module != nodelib.ModuleUnknown && desc.Module != fn.Module {
		return store.FirmwareImage{}, ErrFirmwareDescMismatch
	}
	descVer := int(desc.FWVersionMajor)<<8 | int(desc.FWVersionMinor)
	if descVer != 0 && descVer != fn.Version {
		return store.FirmwareImage{}, ErrFirmwareDescMismatch
	}

	if err := os.MkdirAll(repoDir, 0o755); err != nil {
		return store.FirmwareImage{}, err
	}
	// Canonical on-disk name so the repo dir is self-describing.
	path := filepath.Join(repoDir, nodelib.FirmwareFileName(fn.Module, fn.Version))
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, bin, 0o644); err != nil {
		return store.FirmwareImage{}, err
	}

	prev, hadPrev, _ := s.st.FirmwareImage(ctx, fn.Module)
	if err := os.Rename(tmp, path); err != nil {
		return store.FirmwareImage{}, err
	}
	if err := s.st.PutFirmware(ctx, fn.Module, filename, fn.Version, len(bin), crc, path); err != nil {
		return store.FirmwareImage{}, err
	}
	if hadPrev && prev.ImagePath != "" && prev.ImagePath != path {
		_ = os.Remove(prev.ImagePath)
	}

	return store.FirmwareImage{
		Module: fn.Module.String(), Filename: filename, Version: fn.Version,
		VersionStr: nodelib.VersionString(fn.Version), Size: len(bin), CRC32: crc,
		UploadedTS: time.Now().UnixMilli(), ImagePath: path,
	}, nil
}

// DeleteFirmware drops the held image for a module and removes its file.
func (s *Service) DeleteFirmware(ctx context.Context, mod nodelib.Module) error {
	path, err := s.st.DeleteFirmware(ctx, mod)
	if err != nil {
		return err
	}
	if path != "" {
		_ = os.Remove(path)
	}
	return nil
}

// FirmwareView builds the Firmware page payload: every held image plus one row
// per bus node (and a second row per ControllerNode for its paired Thermostat),
// each classified against the image held for its module.
func (s *Service) FirmwareView(ctx context.Context) (FirmwareView, error) {
	imgs, err := s.st.FirmwareImages(ctx)
	if err != nil {
		return FirmwareView{}, err
	}
	byModule := make(map[string]store.FirmwareImage, len(imgs))
	for _, fi := range imgs {
		byModule[fi.Module] = fi
	}

	roster, err := s.st.Roster(ctx, s.MasterOnline())
	if err != nil {
		return FirmwareView{}, err
	}
	therms, err := s.st.Thermostats(ctx)
	if err != nil {
		return FirmwareView{}, err
	}
	jobs, err := s.st.OtaJobs(ctx, 100)
	if err != nil {
		return FirmwareView{}, err
	}
	pending := make(map[[2]any]string) // {nodeID, target} -> job state
	for _, j := range jobs {
		if j.State == "done" || j.State == "error" {
			continue
		}
		pending[[2]any{j.NodeID, j.Target}] = j.State
	}

	targets := []FwTarget{s.mainControllerTarget(byModule, pending)}
	for _, n := range roster {
		targets = append(targets, s.nodeTarget(n, byModule, pending))
		if n.Module == nodelib.ModuleControllerNode.String() {
			targets = append(targets, s.thermostatTarget(n, therms[n.ID], byModule, pending))
		}
	}
	return FirmwareView{Images: imgs, Targets: targets}, nil
}

// mainControllerTarget is the bus master's own row (node id 0). Its running
// version comes from UplinkHello, not the roster; a push relays as an OTA with
// targetNodeId = 0 (MainController-Server-Link-Spec.md §8).
func (s *Service) mainControllerTarget(byModule map[string]store.FirmwareImage, pending map[[2]any]string) FwTarget {
	mod := nodelib.ModuleMainController.String()
	online := s.MasterOnline()
	fw := s.MainControllerFW()
	t := FwTarget{
		NodeID: 0, Name: "MainController", Module: mod, Target: "node",
		Status: boolWord(online, "online", "offline"), Online: online,
		Installed: fw, InstalledStr: verStr(fw),
	}
	img, hasImg := byModule[mod]
	if hasImg {
		t.Latest, t.LatestStr = img.Version, img.VersionStr
	}
	t.Job = pending[[2]any{0, "node"}]
	t.CanUpdate, t.Reason = updatable(online, hasImg, fw, t.Latest, t.Job)
	return t
}

func boolWord(b bool, t, f string) string {
	if b {
		return t
	}
	return f
}

func (s *Service) nodeTarget(n store.RosterNode, byModule map[string]store.FirmwareImage, pending map[[2]any]string) FwTarget {
	t := FwTarget{
		NodeID: n.ID, Name: n.Name, Module: n.Module, Target: "node",
		Status: n.Status, Online: n.Online,
		Installed: n.FWVersion, InstalledStr: verStr(n.FWVersion),
	}
	img, hasImg := byModule[n.Module]
	if hasImg {
		t.Latest, t.LatestStr = img.Version, img.VersionStr
	}
	t.Job = pending[[2]any{n.ID, "node"}]
	t.CanUpdate, t.Reason = updatable(t.Online, hasImg, n.FWVersion, t.Latest, t.Job)
	return t
}

func (s *Service) thermostatTarget(n store.RosterNode, th store.Thermostat, byModule map[string]store.FirmwareImage, pending map[[2]any]string) FwTarget {
	mod := nodelib.ModuleThermostat.String()
	online := n.Online && th.LinkUp
	status := "link-down"
	if online {
		status = "online"
	} else if !n.Online {
		status = "offline"
	}
	t := FwTarget{
		NodeID: n.ID, Name: thermostatName(n.Name), Module: mod, Target: "thermostat",
		Status: status, Online: online,
		Installed: th.FWVersion, InstalledStr: verStr(th.FWVersion),
	}
	img, hasImg := byModule[mod]
	if hasImg {
		t.Latest, t.LatestStr = img.Version, img.VersionStr
	}
	t.Job = pending[[2]any{n.ID, "thermostat"}]
	t.CanUpdate, t.Reason = updatable(online, hasImg, th.FWVersion, t.Latest, t.Job)
	if !t.CanUpdate && !n.Online {
		t.Reason = "node offline"
	} else if !t.CanUpdate && !th.LinkUp && t.Job == "" && hasImg {
		t.Reason = "offline"
	}
	return t
}

// updatable applies the rule from the brief: a target is updatable only when it
// is online, an image is held, that image is newer than what is installed, and
// no job for it is already queued or running.
func updatable(online, hasImg bool, installed, latest int, job string) (bool, string) {
	switch {
	case job != "":
		return false, "update " + job
	case !online:
		return false, "offline"
	case !hasImg:
		return false, "no image uploaded"
	case installed != 0 && installed >= latest:
		return false, "up to date"
	default:
		return true, ""
	}
}

// EnqueueUpdate queues one push for a target, loading the image from the repo.
func (s *Service) EnqueueUpdate(ctx context.Context, nodeID int, target, otaDir string) (int64, error) {
	mod, err := s.moduleForTarget(ctx, nodeID, target)
	if err != nil {
		return 0, err
	}
	fi, ok, err := s.st.FirmwareImage(ctx, mod)
	if err != nil {
		return 0, err
	}
	if !ok {
		return 0, ErrNoFirmwareImage
	}
	bin, err := os.ReadFile(fi.ImagePath)
	if err != nil {
		return 0, err
	}
	return s.StartOTA(ctx, nodeID, target, fi.Filename, bin, otaDir)
}

// EnqueueUpdateAll queues a push for every currently-updatable target that
// needs the given image module. Returns the job ids created.
func (s *Service) EnqueueUpdateAll(ctx context.Context, module, otaDir string) ([]int64, error) {
	view, err := s.FirmwareView(ctx)
	if err != nil {
		return nil, err
	}
	var ids []int64
	for _, t := range view.Targets {
		if t.Module != module || !t.CanUpdate {
			continue
		}
		id, err := s.EnqueueUpdate(ctx, t.NodeID, t.Target, otaDir)
		if err != nil {
			s.log.Warn("update-all: enqueue", "node", t.NodeID, "target", t.Target, "err", err)
			continue
		}
		ids = append(ids, id)
	}
	return ids, nil
}

func (s *Service) moduleForTarget(ctx context.Context, nodeID int, target string) (nodelib.Module, error) {
	if target == "thermostat" {
		return nodelib.ModuleThermostat, nil
	}
	if nodeID == 0 {
		return nodelib.ModuleMainController, nil // bus master self-update
	}
	nodes, err := s.st.Nodes(ctx)
	if err != nil {
		return nodelib.ModuleUnknown, err
	}
	for _, n := range nodes {
		if n.ID == nodeID {
			if m, ok := nodelib.ModuleByName(n.Module); ok {
				return m, nil
			}
		}
	}
	// Fall back to the expected roster.
	exp, err := s.st.ExpectedNodes(ctx)
	if err != nil {
		return nodelib.ModuleUnknown, err
	}
	for _, e := range exp {
		if e.ID == nodeID {
			if m, ok := nodelib.ModuleByName(e.Module); ok {
				return m, nil
			}
		}
	}
	return nodelib.ModuleUnknown, ErrUnknownNodeModule
}

func verStr(v int) string {
	if v == 0 {
		return "?"
	}
	return nodelib.VersionString(v)
}

func thermostatName(nodeName string) string {
	if nodeName == "" {
		return "Thermostat"
	}
	return nodeName + " · thermostat"
}
