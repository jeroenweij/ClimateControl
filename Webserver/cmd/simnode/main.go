// Command simnode pretends to be a MainController: it connects to the server's
// uplink port, sends UplinkHello + a roster, and streams plausible node
// readings. Use it to exercise the server without hardware.
//
//	simnode -addr 127.0.0.1:9000 -token <32-hex> -controllers 3 -temps 1
package main

import (
	"encoding/hex"
	"flag"
	"log"
	"math"
	"math/rand"
	"net"
	"time"

	"github.com/jweij/climatecontrol/webserver/internal/nodelib"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:9000", "server uplink address")
	tokenHex := flag.String("token", "", "32-hex-char uplink token (from the server's config.json)")
	controllers := flag.Int("controllers", 3, "number of ControllerNodes to simulate")
	temps := flag.Int("temps", 1, "number of TemperatureNodes to simulate")
	period := flag.Duration("period", 3*time.Second, "reporting interval")
	flag.Parse()

	tok, err := hex.DecodeString(*tokenHex)
	if err != nil || len(tok) != 16 {
		log.Fatal("need -token as 32 hex characters (see the server's config.json)")
	}
	var token [16]byte
	copy(token[:], tok)

	for {
		if err := run(*addr, token, *controllers, *temps, *period); err != nil {
			log.Printf("disconnected: %v — retrying in 2s", err)
			time.Sleep(2 * time.Second)
		}
	}
}

type simNode struct {
	id     int
	module nodelib.Module
	temp   float64
	setpt  float64
}

func run(addr string, token [16]byte, nCtrl, nTemp int, period time.Duration) error {
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		return err
	}
	defer conn.Close()
	log.Printf("connected to %s", addr)

	var nodes []simNode
	id := 1
	for i := 0; i < nCtrl; i++ {
		nodes = append(nodes, simNode{id: id, module: nodelib.ModuleControllerNode, temp: 20 + rand.Float64()*2, setpt: 21})
		id++
	}
	for i := 0; i < nTemp; i++ {
		nodes = append(nodes, simNode{id: id, module: nodelib.ModuleTemperatureNode, temp: 8 + rand.Float64()*4})
		id++
	}

	write := func(f nodelib.Frame) error {
		raw, err := nodelib.Encode(f)
		if err != nil {
			return err
		}
		_, err = conn.Write(raw)
		return err
	}

	// UplinkHello.
	hello := make([]byte, 0, 23)
	hello = appendU16(hello, 0x0001)
	hello = appendU32(hello, 0)
	hello = append(hello, byte(len(nodes)))
	hello = append(hello, token[:]...)
	if err := write(nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointUplinkHello, Operation: nodelib.OpReport, Data: hello}); err != nil {
		return err
	}

	// Roster.
	now := uint32(0)
	for _, n := range nodes {
		e := []byte{byte(n.id), byte(n.module), 0}
		e = appendU32(e, now)
		if err := write(nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpReport, Data: e}); err != nil {
			return err
		}
	}
	_ = write(nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointRoster, Operation: nodelib.OpReport,
		Data: append([]byte{0xFF, 0, 0}, 0, 0, 0, 0)})

	// Drain inbound (Get roster / Set overrides) so the socket doesn't stall.
	go func() {
		df := nodelib.NewDeframer()
		buf := make([]byte, 1024)
		for {
			k, err := conn.Read(buf)
			if err != nil {
				return
			}
			for _, f := range df.Push(buf[:k]) {
				if f.Operation == nodelib.OpSet && f.Node != 0 {
					for i := range nodes {
						if nodes[i].id == int(f.Node) && f.Endpoint == nodelib.EndpointRoomSetpoint && len(f.Data) >= 2 {
							nodes[i].setpt = float64(int16(uint16(f.Data[0])|uint16(f.Data[1])<<8)) / 100
							log.Printf("node %d setpoint -> %.2f", f.Node, nodes[i].setpt)
						}
					}
				}
			}
		}
	}()

	tick := time.NewTicker(period)
	defer tick.Stop()
	status := time.NewTicker(10 * time.Second)
	defer status.Stop()
	uptime := uint32(0)

	for {
		select {
		case <-tick.C:
			for i := range nodes {
				n := &nodes[i]
				n.temp += (rand.Float64() - 0.5) * 0.3
				if n.module == nodelib.ModuleControllerNode {
					n.temp += (n.setpt - n.temp) * 0.05
					emit(write, n.id, nodelib.EndpointRoomTemp, i16(n.temp*100))
					emit(write, n.id, nodelib.EndpointRoomSetpoint, i16(n.setpt*100))
					damper := clamp(50+(n.setpt-n.temp)*30, 0, 100)
					emit(write, n.id, nodelib.EndpointDamperActual, []byte{byte(damper)})
					emit(write, n.id, nodelib.EndpointRoomLink, []byte{1})
				} else {
					supply := n.temp + 18
					emit(write, n.id, nodelib.EndpointSupplyTemp, i16(supply*100))
					emit(write, n.id, nodelib.EndpointReturnTemp, i16(n.temp*100))
					emit(write, n.id, nodelib.EndpointSensorStatus, []byte{0x03})
				}
			}
		case <-status.C:
			uptime += 10
			ms := make([]byte, 0, 23)
			ms = appendU32(ms, uptime*4)
			ms = appendU32(ms, 0)
			ms = appendU32(ms, 0)
			ms = appendU32(ms, 0)
			ms = appendU32(ms, 0)
			rssi := int8(-52)
			ms = append(ms, byte(rssi)) // wifiRssi as int8
			ms = appendU16(ms, 3200)
			if err := write(nodelib.Frame{Node: 0, Endpoint: nodelib.EndpointMainStatus, Operation: nodelib.OpReport, Data: ms}); err != nil {
				return err
			}
		}
	}
}

func emit(w func(nodelib.Frame) error, node int, ep nodelib.Endpoint, data []byte) {
	_ = w(nodelib.Frame{Node: byte(node), Endpoint: ep, Operation: nodelib.OpReport, Data: data})
}

func i16(f float64) []byte {
	v := int16(math.Round(f))
	return []byte{byte(v), byte(uint16(v) >> 8)}
}
func appendU16(b []byte, v uint16) []byte { return append(b, byte(v), byte(v>>8)) }
func appendU32(b []byte, v uint32) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}
func clamp(v, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, v))
}
