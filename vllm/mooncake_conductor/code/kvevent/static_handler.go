// Copyright 2025 AIBrix Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//go:build zmq
// +build zmq

package kvevent

import (
	"context"
	"encoding/binary"
	"fmt"
	"time"

	"log/slog"

	"conductor.local/kvcache"
)

type SyncIndexProvider struct {
}

type SyncIndexer struct {
}

func (m *SyncIndexProvider) GetSyncIndexer(ctx context.Context) (SyncIndexer, error) {
	slog.Info("GetSyncIndexer")
	return SyncIndexer{}, nil
}

// staticEventHandler adapts the generic EventHandler interface for StaticManager.
// It is instantiated in static_manager.go but implemented here to keep files clean.
type staticEventHandler struct {
	manager   *StaticManager
	svcName   string
	modelName string
	loraID    int64
}

// HandleEvent processes incoming events from.
func (h *staticEventHandler) HandleEvent(event kvcache.KVEvent) error {
	// 1. Lifecycle check
	h.manager.mu.RLock()
	if h.manager.stopped {
		h.manager.mu.RUnlock()
		return fmt.Errorf("manager stopped")
	}
	h.manager.mu.RUnlock()

	// 2. Create context for processing
	ctx, cancel := context.WithTimeout(h.manager.ctx, 10*time.Second)
	defer cancel()

	// 3. Get Indexer
	// Retrieves the indexer from the manager's provider.
	// indexer, err := h.manager.syncProvider.GetSyncIndexer(ctx)
	// if err != nil {
	// 	return err
	// }

	slog.Info("Successfully")

	// 4. Dispatch event
	// The decodes messages into specific event types (BlockStored/Removed).
	// We pass these directly to the Indexer logic.
	switch e := event.(type) {
	case *kvcache.BlockStoredEvent:
		slog.Info("[%s] BlockStored: %d blocks", h.svcName, len(e.BlockHashes))
		return h.handleBlockStored(ctx, e)
	case *kvcache.BlockRemovedEvent:
		slog.Info("[%s] BlockRemoved: %d blocks", h.svcName, len(e.BlockHashes))
		return h.handleBlockRemoved(ctx, e)

	default:
		slog.Warn("Unknown event type: %T", event)
		return nil
	}
}

func (h *staticEventHandler) handleBlockStored(ctx context.Context, event *kvcache.BlockStoredEvent) error {
	// Get sync indexer

	// Convert to sync event
	syncEvent := BlockStoredEvent{
		BlockHashes:     event.BlockHashes,
		ModelName:       h.modelName,
		LoraID:          h.loraID,
		SourcePod:       h.svcName,
		ParentBlockHash: event.ParentBlockHash,
		Tokens:          convertTokenIDs(event.TokenIDs),
	}

	slog.Debug("Sync event generated (not sent)",
		"model", syncEvent.ModelName,
		"lora_id", syncEvent.LoraID,
	)

	return nil
}

func (h *staticEventHandler) handleBlockRemoved(ctx context.Context, event *kvcache.BlockRemovedEvent) error {

	// Convert to sync event
	syncEvent := BlockRemovedEvent{
		BlockHashes: event.BlockHashes,
		ModelName:   h.modelName,
		LoraID:      h.loraID,
		SourcePod:   h.svcName,
	}

	slog.Debug("Sync event generated (not sent)",
		"model", syncEvent.ModelName,
		"lora_id", syncEvent.LoraID,
	)

	return nil
}

// convertTokenIDs converts [][]int32 to [][]byte
func convertTokenIDs(tokenIDs [][]int32) [][]byte {
	result := make([][]byte, len(tokenIDs))
	for i, ids := range tokenIDs {
		result[i] = tokenIDsToBytes(ids)
	}
	return result
}

// tokenIDsToBytes converts []int32 to []byte
func tokenIDsToBytes(tokenIDs []int32) []byte {
	bytes := make([]byte, len(tokenIDs)*4)
	for i, id := range tokenIDs {
		binary.BigEndian.PutUint32(bytes[i*4:], uint32(id))
	}
	return bytes
}
