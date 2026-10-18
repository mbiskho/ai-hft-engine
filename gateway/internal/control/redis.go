// Package control is the operator's kill switch. Publishing "kill" on the
// Redis channel hft:control injects a KillSwitch event into the same stream
// the engine reads market data from. The engine sees it in order, on its own
// thread, with no lock and no second input to poll.
//
//	redis-cli PUBLISH hft:control kill
package control

import (
	"context"
	"log/slog"
	"strings"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/mbiskho/ai-hft-engine/gateway/internal/event"
)

const Channel = "hft:control"

func Listen(ctx context.Context, url string, symbolID uint16, log *slog.Logger, emit func(event.Event)) error {
	opt, err := redis.ParseURL(url)
	if err != nil {
		return err
	}
	rdb := redis.NewClient(opt)
	defer rdb.Close()
	ps := rdb.Subscribe(ctx, Channel)
	defer ps.Close()
	for {
		select {
		case <-ctx.Done():
			return nil
		case msg, ok := <-ps.Channel():
			if !ok {
				return nil
			}
			if strings.TrimSpace(strings.ToLower(msg.Payload)) == "kill" {
				now := uint64(time.Now().UnixNano())
				log.Warn("KILL SWITCH requested via redis", "channel", Channel)
				emit(event.Event{Type: event.KillSwitch, SymbolID: symbolID, ExchangeTsNs: now, RecvTsNs: now})
			}
		}
	}
}
