# ClimateControl server.
#
#   make build        build ./bin/ccserver for this host
#   make build-arm64  cross-compile for a 64-bit ARM box (Raspberry Pi 3/4/5, 64-bit OS)
#   make build-armv7  cross-compile for a 32-bit ARM box (older Pi / 32-bit OS)
#   make run          run against ./data with a local config.json (created on first run)
#   make sim          run the node simulator against a local server
#   make test         go vet + go test
#   make dist         tar up the binary + service unit + example config for deployment

BIN      ?= ccserver
MODULE   := github.com/jweij/climatecontrol/webserver
VERSION  := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
LDFLAGS  := -s -w -X main.version=$(VERSION)
GOFLAGS  := -trimpath

.PHONY: build build-arm64 build-armv7 run sim test dist clean

build:
	CGO_ENABLED=0 go build $(GOFLAGS) -ldflags '$(LDFLAGS)' -o bin/$(BIN) ./cmd/ccserver

build-arm64:
	CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build $(GOFLAGS) -ldflags '$(LDFLAGS)' -o bin/$(BIN)-linux-arm64 ./cmd/ccserver

build-armv7:
	CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build $(GOFLAGS) -ldflags '$(LDFLAGS)' -o bin/$(BIN)-linux-armv7 ./cmd/ccserver

build-amd64:
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build $(GOFLAGS) -ldflags '$(LDFLAGS)' -o bin/$(BIN)-linux-amd64 ./cmd/ccserver

run: build
	./bin/$(BIN) -config config.json -debug

sim:
	@test -f config.json || { echo "run the server once first to generate config.json"; exit 1; }
	go run ./cmd/simnode -token $$(python3 -c "import json;print(json.load(open('config.json'))['uplinkToken'])")

test:
	go vet ./...
	go test ./...

dist: build-amd64 build-arm64
	@mkdir -p dist
	@for arch in amd64 arm64; do \
		d=dist/climatecontrol-server-$(VERSION)-linux-$$arch; \
		rm -rf $$d && mkdir -p $$d; \
		cp bin/$(BIN)-linux-$$arch $$d/ccserver; \
		cp deploy/ccserver.service deploy/config.example.json $$d/; \
		cp README.md $$d/; \
		tar -C dist -czf $$d.tar.gz $$(basename $$d); \
		echo "wrote $$d.tar.gz"; \
	done

clean:
	rm -rf bin dist
