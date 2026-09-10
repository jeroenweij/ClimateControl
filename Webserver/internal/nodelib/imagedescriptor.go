package nodelib

import (
	"encoding/binary"
	"errors"
	"hash/crc32"
)

// ImageDescriptor mirrors Board::ImageDescriptor in
// Software/Lib/Board/ImageDescriptor.h: a fixed 32-byte record at offset 0xC0
// from the application base, describing an OTA image.
type ImageDescriptor struct {
	Magic          uint32
	HeaderVersion  uint16
	Module         Module
	ImageSize      uint32 // bytes from app base to end of image, incl. trailing CRC-32
	FWVersionMajor uint16
	FWVersionMinor uint16
	Flags          uint16
	BuildID        uint32
}

const (
	imageMagic          = 0x43436D67 // 'CCmg'
	imageDescriptorOff  = 0xC0
	imageDescriptorSize = 32
	flagCRCPresent      = 1 << 0
)

// ErrBadImage is returned by ParseImage when the buffer is not a valid module
// image.
var ErrBadImage = errors.New("nodelib: not a valid ClimateControl image")

// ParseImage validates an uploaded .bin (raw application image, base at
// offset 0). It checks the descriptor magic and, when the CRC-present flag is
// set, the trailing IEEE CRC-32 over [0, imageSize-4). It returns the
// descriptor and the CRC-32 the OTA sequence should announce (computed over
// the whole file when no descriptor size is trusted).
func ParseImage(bin []byte) (ImageDescriptor, uint32, error) {
	if len(bin) < imageDescriptorOff+imageDescriptorSize {
		return ImageDescriptor{}, 0, ErrBadImage
	}
	d := bin[imageDescriptorOff : imageDescriptorOff+imageDescriptorSize]
	desc := ImageDescriptor{
		Magic:          binary.LittleEndian.Uint32(d[0:]),
		HeaderVersion:  binary.LittleEndian.Uint16(d[4:]),
		Module:         Module(binary.LittleEndian.Uint16(d[6:])),
		ImageSize:      binary.LittleEndian.Uint32(d[8:]),
		FWVersionMajor: binary.LittleEndian.Uint16(d[12:]),
		FWVersionMinor: binary.LittleEndian.Uint16(d[14:]),
		Flags:          binary.LittleEndian.Uint16(d[16:]),
		BuildID:        binary.LittleEndian.Uint32(d[20:]),
	}
	if desc.Magic != imageMagic {
		return desc, 0, ErrBadImage
	}

	size := int(desc.ImageSize)
	if desc.Flags&flagCRCPresent != 0 {
		if size < 8 || size > len(bin) {
			return desc, 0, ErrBadImage
		}
		want := binary.LittleEndian.Uint32(bin[size-4 : size])
		got := crc32.ChecksumIEEE(bin[:size-4])
		if got != want {
			return desc, 0, ErrBadImage
		}
		return desc, want, nil
	}

	// Dev image without a finalized CRC: OTA still needs a value to announce.
	// Use the descriptor size if plausible, else the whole file.
	if size <= 0 || size > len(bin) {
		size = len(bin)
		desc.ImageSize = uint32(size)
	}
	return desc, crc32.ChecksumIEEE(bin[:size]), nil
}
