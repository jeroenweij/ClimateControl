package nodelib

import (
	"regexp"
	"strconv"
	"strings"
)

// FirmwareName is a parsed OTA-image filename. The server's firmware repository
// keeps one image per module and identifies both the module and the version
// from the filename, which the build system produces in a fixed shape:
//
//	<Module>_<major>.<minor>.bin      e.g. ControllerNode_1.0.bin
//	<Module>_<major>.<minor>.<patch>.bin  (patch is accepted but ignored)
//
// <Module> matches a board-type name (Module.String()) case-insensitively.
// Anything else is rejected by ParseFirmwareName.
type FirmwareName struct {
	Module  Module
	Major   int
	Minor   int
	Version int // Major<<8 | Minor, matching the descriptor / DB encoding
}

// VersionString renders "major.minor".
func VersionString(version int) string {
	return strconv.Itoa(version>>8) + "." + strconv.Itoa(version&0xFF)
}

var firmwareNameRe = regexp.MustCompile(`^([A-Za-z]+)[_-]v?([0-9]+)\.([0-9]+)(?:\.[0-9]+)?\.bin$`)

// ParseFirmwareName validates name against the convention and returns the
// module and version it encodes. ok is false for any filename that does not
// match or whose module component is not a known board type.
func ParseFirmwareName(name string) (FirmwareName, bool) {
	m := firmwareNameRe.FindStringSubmatch(name)
	if m == nil {
		return FirmwareName{}, false
	}
	mod, ok := moduleByLowerName[strings.ToLower(m[1])]
	if !ok || mod == ModuleUnknown {
		return FirmwareName{}, false
	}
	major, _ := strconv.Atoi(m[2])
	minor, _ := strconv.Atoi(m[3])
	if major > 0xFF || minor > 0xFF {
		return FirmwareName{}, false
	}
	return FirmwareName{Module: mod, Major: major, Minor: minor, Version: major<<8 | minor}, true
}

// FirmwareFileName builds the canonical filename for a module + version.
func FirmwareFileName(mod Module, version int) string {
	return mod.String() + "_" + VersionString(version) + ".bin"
}

var moduleByLowerName = func() map[string]Module {
	m := make(map[string]Module, len(moduleNames))
	for mod, n := range moduleNames {
		m[strings.ToLower(n)] = mod
	}
	return m
}()
