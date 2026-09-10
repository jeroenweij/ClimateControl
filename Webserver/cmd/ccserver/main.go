// Command ccserver is the ClimateControl building server: it accepts the
// MainController TCP connection, decodes NodeLib frames into a SQLite history,
// and serves the operator web application (SPA + JSON API + WebSocket).
// See MainController-Server-Link-Spec.md.
package main

import (
	"context"
	"errors"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	webserver "github.com/jweij/climatecontrol/webserver"
	"github.com/jweij/climatecontrol/webserver/internal/config"
	"github.com/jweij/climatecontrol/webserver/internal/httpapi"
	"github.com/jweij/climatecontrol/webserver/internal/hub"
	"github.com/jweij/climatecontrol/webserver/internal/service"
	"github.com/jweij/climatecontrol/webserver/internal/store"
	"github.com/jweij/climatecontrol/webserver/internal/uplink"
)

// version is set at build time via -ldflags "-X main.version=...".
var version = "dev"

func main() {
	cfgPath := flag.String("config", "config.json", "path to the JSON config file")
	debug := flag.Bool("debug", false, "verbose logging")
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()

	if *showVersion {
		println("ccserver", version)
		return
	}

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	if err := run(*cfgPath, log); err != nil {
		log.Error("fatal", "err", err)
		os.Exit(1)
	}
}

func run(cfgPath string, log *slog.Logger) error {
	cfg, err := config.Load(cfgPath)
	if err != nil {
		return err
	}
	token, err := cfg.Token()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(cfg.DataDir, 0o755); err != nil {
		return err
	}
	log.Info("ccserver starting", "version", version, "http", cfg.HTTPAddr,
		"uplink", cfg.UplinkAddr, "data", cfg.DataDir)

	st, err := store.Open(cfg.SQLitePath())
	if err != nil {
		return err
	}
	defer st.Close()

	hb := hub.New()
	svc := service.New(st, hb, log)

	up := uplink.New(token, svc, func(bool) {}, log)
	svc.SetSender(up)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Uplink TCP listener.
	uplinkErr := make(chan error, 1)
	go func() { uplinkErr <- up.ListenAndServe(ctx, cfg.UplinkAddr) }()

	// HTTP server.
	api := httpapi.New(svc, webserver.WebFS(), cfg.AssetDir(), log)
	httpSrv := &http.Server{
		Addr:         cfg.HTTPAddr,
		Handler:      api.Handler(),
		ReadTimeout:  15 * time.Second,
		WriteTimeout: 0, // WebSocket needs an unbounded write timeout
		IdleTimeout:  60 * time.Second,
	}
	httpErr := make(chan error, 1)
	go func() {
		log.Info("http listening", "addr", cfg.HTTPAddr)
		httpErr <- httpSrv.ListenAndServe()
	}()

	select {
	case <-ctx.Done():
		log.Info("shutting down")
	case err := <-uplinkErr:
		if err != nil {
			return err
		}
	case err := <-httpErr:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return err
		}
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(shutdownCtx)
	return nil
}
