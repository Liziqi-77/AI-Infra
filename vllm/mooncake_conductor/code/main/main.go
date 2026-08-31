//go:build zmq
// +build zmq

package main

import (
	"context"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/vllm-project/aibrix/pkg/cache/kvcache"
	"github.com/vllm-project/aibrix/pkg/kvevent"
)

// -----------------------------------------------------------------------------
// 1. Mock Implementation (模拟 Indexer)
// -----------------------------------------------------------------------------

// DemoSyncProvider implements SyncIndexProvider
type DemoSyncProvider struct{}

func (m *DemoSyncProvider) GetSyncIndexer(ctx context.Context) (kvevent.SyncIndexer, error) {
	return &DemoIndexer{}, nil
}

// DemoIndexer logs received events to stdout
type DemoIndexer struct{}

func (i *DemoIndexer) ProcessBlockStored(ctx context.Context, event interface{}) error {
	// In a real app, 'event' would be converted to internal struct.
	// Here we cast it to the source event type for logging.
	if e, ok := event.(*kvcache.BlockStoredEvent); ok {
		slog.Info(">> [Indexer] BlockStored",
			"pod", e.PodName,
			"model", e.ModelName,
			"count", len(e.BlockHashes),
			"first_hash", fmtFirstHash(e.BlockHashes))
	}
	return nil
}

func (i *DemoIndexer) ProcessBlockRemoved(ctx context.Context, event interface{}) error {
	if e, ok := event.(*kvcache.BlockRemovedEvent); ok {
		slog.Info("<< [Indexer] BlockRemoved",
			"pod", e.PodName,
			"count", len(e.BlockHashes))
	}
	return nil
}

func fmtFirstHash(hashes []int64) string {
	if len(hashes) > 0 {
		return fmt.Sprintf("%d", hashes[0])
	}
	return "none"
}

import "fmt" // Added missing import

// -----------------------------------------------------------------------------
// 2. Main Entry
// -----------------------------------------------------------------------------

func main() {
	// Setup structured logging
	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelDebug, // Enable Debug logs to see ZMQ details
	}))
	slog.SetDefault(logger)

	slog.Info("Starting KV Event Manager Demo...")

	// 1. Configuration (Static Service List)
	// You should modify these values to match your vLLM environment
	services := []kvevent.ServiceConfig{
		{
			Name:      "vllm-local",
			IP:        "127.0.0.1", // Assuming vLLM is running locally
			Port:      5557,        // Default vLLM ZMQ port
			Type:      kvevent.ServiceTypeVLLM,
			ModelName: "llama-2-7b",
			LoraID:    -1,
		},
		// Add more services here...
	}

	// 2. Initialize Dependencies
	provider := &DemoSyncProvider{}

	// 3. Create Manager
	manager := kvevent.NewStaticManager(services, provider)

	// 4. Start Manager
	if err := manager.Start(); err != nil {
		slog.Error("Failed to start manager", "error", err)
		os.Exit(1)
	}

	// 5. Wait for Signal (Graceful Shutdown)
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)

	slog.Info("Manager is running. Press Ctrl+C to stop.")
	<-sigChan

	// 6. Shutdown
	slog.Info("Shutting down...")
	manager.Stop()
	slog.Info("Bye!")
}

