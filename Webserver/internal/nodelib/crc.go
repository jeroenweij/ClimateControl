package nodelib

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no input/output reflection,
// no final XOR. This matches the STM32 hardware CRC unit configuration in
// Software/Lib/HAL/Crc.cpp (Poly::Ccitt16) and the frame CRC in
// RS485-Node-Protocol-Spec-STM32G030.md §4.

var crc16Table [256]uint16

func init() {
	for i := 0; i < 256; i++ {
		c := uint16(i) << 8
		for b := 0; b < 8; b++ {
			if c&0x8000 != 0 {
				c = (c << 1) ^ 0x1021
			} else {
				c <<= 1
			}
		}
		crc16Table[i] = c
	}
}

// CRC16 computes the frame CRC over buf (NODE..DATA inclusive).
func CRC16(buf []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, b := range buf {
		crc = (crc << 8) ^ crc16Table[byte(crc>>8)^b]
	}
	return crc
}

// CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no input/output
// reflection, no final XOR. This matches the STM32 hardware CRC unit's
// *native* configuration in Software/Lib/HAL/Crc.cpp (Poly::Ieee32) and the
// whole-image check in Node-Flash-Layout-and-Bootloader-Spec.md §4 --
// deliberately NOT the same algorithm as Go's stdlib hash/crc32
// (crc32.ChecksumIEEE) or zlib/binascii's crc32, which are all the
// *reflected* IEEE 802.3 variant and produce a different result over the
// same bytes. Do not substitute one for the other.

var crc32Table [256]uint32

func init() {
	for i := 0; i < 256; i++ {
		c := uint32(i) << 24
		for b := 0; b < 8; b++ {
			if c&0x80000000 != 0 {
				c = (c << 1) ^ 0x04C11DB7
			} else {
				c <<= 1
			}
		}
		crc32Table[i] = c
	}
}

// CRC32 computes the STM32-native (non-reflected) CRC-32 over buf -- the
// value FirmwareSlave::HandleEnd() checks the image against.
func CRC32(buf []byte) uint32 {
	crc := uint32(0xFFFFFFFF)
	for _, b := range buf {
		crc = (crc << 8) ^ crc32Table[byte(crc>>24)^b]
	}
	return crc
}
