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
	"fmt"
	"sync"
	"time"

	"k8s.io/klog/v2"

	"github.com/vllm-project/aibrix/pkg/cache/kvcache"
	"github.com/vllm-project/aibrix/pkg/utils"
)

// StaticManager manages KV event subscriptions for a fixed set of services.
// It is designed for scenarios where vLLM/Mooncake instances are static and
// do not require dynamic discovery via Kubernetes API.
type StaticManager struct {
	// Dependencies injected via interfaces
	// Note: We reuse the existing SyncIndexProvider interface
	syncProvider SyncIndexProvider

	// Configuration
	services []ServiceConfig

	// Subscriber management
	// Using utils.SyncMap for type safety with Generics
	subscribers utils.SyncMap[string, *kvcache.ZMQClient]

	// Lifecycle management
	ctx     context.Context
	cancel  context.CancelFunc
	mu      sync.RWMutex
	stopped bool
}

// NewStaticManager creates a new static KV event manager.
func NewStaticManager(services []ServiceConfig, syncProvider SyncIndexProvider) *StaticManager {
	ctx, cancel := context.WithCancel(context.Background())
	return &StaticManager{
		services:     services,
		syncProvider: syncProvider,
		ctx:          ctx,
		cancel:       cancel,
	}
}

// Start initializes the manager and establishes subscriptions for all configured services.
// Unlike the dynamic Manager, this method attempts to connect to all services immediately.
func (m *StaticManager) Start() error {
	klog.Info("Starting Static KV Event Manager...")

	// 1. Verify SyncIndexer availability
	// Even with static configuration, the core indexing component must be ready.
	initCtx, cancel := context.WithTimeout(m.ctx, 10*time.Second)
	defer cancel()

	if _, err := m.syncProvider.GetSyncIndexer(initCtx); err != nil {
		return fmt.Errorf("sync indexer not ready: %w", err)
	}

	// 2. Subscribe to all services concurrently
	var wg sync.WaitGroup
	errChan := make(chan error, len(m.services))

	for _, svc := range m.services {
		wg.Add(1)
		go func(service ServiceConfig) {
			defer wg.Done()
			if err := m.subscribeToService(service); err != nil {
				klog.Errorf("Failed to initiate subscription for %s service %s (%s): %v",
					service.Type, service.Name, service.IP, err)
				// We don't block startup on individual subscription failures,
				// as ZMQClient handles reconnection internally.
				errChan <- fmt.Errorf("failed to subscribe to %s: %w", service.Name, err)
			}
		}(svc)
	}

	// Wait for all subscription attempts to complete (or fail)
	wg.Wait()
	close(errChan)

	// Log summary
	failureCount := len(errChan)
	successCount := len(m.services) - failureCount
	klog.Infof("Static KV Event Manager started. Subscriptions: %d success, %d failed (will retry internally)",
		successCount, failureCount)

	return nil
}

// Stop gracefully shuts down the manager and all subscriptions.
func (m *StaticManager) Stop() {
	m.mu.Lock()
	if m.stopped {
		m.mu.Unlock()
		return
	}
	m.stopped = true
	m.mu.Unlock()

	klog.Info("Stopping Static KV Event Manager")

	// 1. Cancel context to signal shutdown to any internal routines
	m.cancel()

	// 2. Stop all ZMQ clients
	// With utils.SyncMap, 'client' is already *kvcache.ZMQClient, no type assertion needed
	m.subscribers.Range(func(key string, client *kvcache.ZMQClient) bool {
		client.Stop()
		klog.Infof("Stopped subscription for %s", key)
		return true
	})
}

// subscribeToService establishes a ZMQ subscription for a single service.
func (m *StaticManager) subscribeToService(svc ServiceConfig) error {
	// check duplication
	if _, exists := m.subscribers.Load(svc.Name); exists {
		return nil
	}

	// Create handler adapter
	handler := &staticEventHandler{
		manager:   m,
		svcName:   svc.Name,
		modelName: svc.ModelName,
		loraID:    svc.LoraID,
	}

	// Configure ZMQ Client
	// We reuse the existing ZMQClientConfig but adapt it for our static usage.
	zmqConfig := &kvcache.ZMQClientConfig{
		PodKey:         svc.Name, // Use Service Name as key
		PodIP:          svc.IP,
		PubPort:        svc.Port,
		ModelName:      svc.ModelName,
		PollTimeout:    100 * time.Millisecond,
		ReplayTimeout:  5 * time.Second,
		ReconnectDelay: 1 * time.Second,
		// RouterPort is typically PubPort + 1, but should be configurable if different
		RouterPort: svc.Port + 1,
	}

	// Create and start client
	client := kvcache.NewZMQClient(zmqConfig, handler)
	if err := client.Start(); err != nil {
		return fmt.Errorf("failed to start ZMQ client: %w", err)
	}

	m.subscribers.Store(svc.Name, client)
	klog.Infof("Successfully subscribed to %s service: %s at %s:%d",
		svc.Type, svc.Name, svc.IP, svc.Port)

	return nil
}

// staticEventHandler adapts the generic EventHandler interface for StaticManager.
type staticEventHandler struct {
	manager   *StaticManager
	svcName   string
	modelName string
	loraID    int64
}

// HandleEvent processes incoming events from ZMQClient.
func (h *staticEventHandler) HandleEvent(event kvcache.KVEvent) error {
	// 1. Lifecycle check
	h.manager.mu.RLock()
	if h.manager.stopped {
		h.manager.mu.RUnlock()
		return fmt.Errorf("manager stopped")
	}
	h.manager.mu.RUnlock()

	// 2. Create context for processing
	ctx, cancel := context.WithTimeout(h.manager.ctx, 5*time.Second)
	defer cancel()

	// 3. Get Indexer
	// Note: In a static scenario, we might optimize this by caching the indexer,
	// but retrieving it via provider ensures we handle potential reloads/errors.
	indexer, err := h.manager.syncProvider.GetSyncIndexer(ctx)
	if err != nil {
		return err
	}

	// 4. Dispatch event
	// The ZMQClient decodes messages into specific event types (BlockStored/Removed).
	// We pass these directly to the Indexer logic.
	switch e := event.(type) {
	case *kvcache.BlockStoredEvent:
		// Mock call to satisfy interface
		klog.V(4).Infof("[%s] BlockStored: %d blocks", h.svcName, len(e.BlockHashes))
		// In real implementation: return indexer.ProcessBlockStored(ctx, *e)
		return nil

	case *kvcache.BlockRemovedEvent:
		klog.V(4).Infof("[%s] BlockRemoved: %d blocks", h.svcName, len(e.BlockHashes))
		// In real implementation: return indexer.ProcessBlockRemoved(ctx, *e)
		return nil

	default:
		return nil
	}
}
