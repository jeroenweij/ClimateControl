package webserver

import (
	"embed"
	"io/fs"
)

//go:embed all:web
var webFiles embed.FS

// WebFS is the embedded SPA bundle, rooted so that index.html is at "/".
func WebFS() fs.FS {
	sub, err := fs.Sub(webFiles, "web")
	if err != nil {
		panic(err)
	}
	return sub
}
