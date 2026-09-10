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
