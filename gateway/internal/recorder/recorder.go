// Package recorder appends raw 64-byte events to a file, the input format for
// `hft-engine --input` replays and research/dataset.py.
package recorder

import (
	"bufio"
	"os"
	"sync"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

type File struct {
	mu  sync.Mutex
	f   *os.File
	w   *bufio.Writer
	buf [event.Size]byte
}

func Open(path string) (*File, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	return &File{f: f, w: bufio.NewWriterSize(f, 1<<20)}, nil
}

func (r *File) Write(e event.Event) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	e.Encode(r.buf[:])
	_, err := r.w.Write(r.buf[:])
	return err
}

func (r *File) Close() error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if err := r.w.Flush(); err != nil {
		return err
	}
	return r.f.Close()
}
