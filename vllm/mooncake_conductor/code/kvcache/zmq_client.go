//go:build zmq
// +build zmq

package kvcache

import (
	"context"
	"encoding/binary"
	"fmt"
	"log/slog"
	"sync"
	"time"

	zmq "github.com/pebbe/zmq4"
)

// StaticZMQClient is a simplified ZMQ client optimized for static service deployments.
type StaticZMQClient struct {
	config *ZMQClientConfig

	// ZMQ sockets
	subSocket    *zmq.Socket
	replaySocket *zmq.Socket

	// Event handler
	eventHandler EventHandler

	// State management
	mu             sync.RWMutex
	connected      bool
	lastSeq        int64
	reconnectDelay time.Duration

	// Lifecycle
	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// NewStaticZMQClient creates a new client instance.
func NewStaticZMQClient(config *ZMQClientConfig, handler EventHandler) *StaticZMQClient {
	ctx, cancel := context.WithCancel(context.Background())
	return &StaticZMQClient{
		config:         config,
		eventHandler:   handler,
		lastSeq:        -1,
		reconnectDelay: config.ReconnectDelay,
		ctx:            ctx,
		cancel:         cancel,
	}
}

// Start initiates the connection and background event consumption loop.
func (c *StaticZMQClient) Start() error {
	// Attempt initial connection
	if err := c.Connect(); err != nil {
		return fmt.Errorf("initial connection failed: %w", err)
	}

	c.wg.Add(1)
	go c.loop()

	slog.Info("Static ZMQ client started", "service", c.config.PodKey)
	return nil
}

// Stop gracefully shuts down the client.
func (c *StaticZMQClient) Stop() {
	c.cancel()
	c.wg.Wait()

	c.mu.Lock()
	c.cleanupSockets()
	c.mu.Unlock()

	slog.Info("Static ZMQ client stopped", "service", c.config.PodKey)
}

// loop is the main background loop handling events and reconnections.
// Simplified: Fixed reconnect interval, single loop structure.
func (c *StaticZMQClient) loop() {
	defer c.wg.Done()

	for {
		// Check if we should stop
		select {
		case <-c.ctx.Done():
			return
		default:
		}

		// 1. If disconnected, wait for ticker then try to reconnect
		if !c.isConnected() {
			c.handleReconnect()
			continue
		}

		// 2. If connected, consume events
		if err := c.consume(); err != nil {
			slog.Error("Consumption error", "service", c.config.PodKey, "error", err)
			c.markDisconnected()
		}
	}
}

func (c *StaticZMQClient) handleReconnect() {
	slog.Info("Attempting to reconnect to %s after %v", c.config.PodKey, c.reconnectDelay)

	ticker := time.NewTicker(c.config.ReconnectDelay)
	defer ticker.Stop()

	select {
	case <-c.ctx.Done():
		return
	case <-ticker.C:
	}

	if err := c.Connect(); err != nil {
		slog.Error("Reconnect failed", "service", c.config.PodKey, "error", err)
	}

	// Reconnected! Request replay from last known sequence
	lastSeq := c.getLastSequence()
	if lastSeq >= 0 {
		slog.Info("Reconnected", "service", c.config.PodKey, "resuming_from", lastSeq+1)
		if err := c.requestReplay(lastSeq + 1); err != nil {
			slog.Warn("Failed to request replay after reconnect for %s: %v", c.config.PodKey, err)
		}
	}

}

// Connect establishes the ZMQ SUB and DEALER sockets.
func (c *StaticZMQClient) Connect() error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.connected {
		return nil
	}

	// Ensure clean state
	c.cleanupSockets()

	// Create Sockets
	sock, err := zmq.NewSocket(zmq.SUB)
	if err != nil {
		return fmt.Errorf("create socket failed: %w", err)
	}

	if err := sock.SetIpv6(true); err != nil {
		_ = sock.Close()
		return fmt.Errorf("failed to enable IPv6 on socket: %w", err)
	}

	endpoint := formatZMQTCPEndpoint(c.config.PodIP, c.config.PubPort)
	if err := sock.Connect(endpoint); err != nil {
		_ = sock.Close()
		return fmt.Errorf("failed to connect to %s: %w", endpoint, err)
	}

	// Important: Subscribe to all topics
	if err := sock.SetSubscribe(""); err != nil {
		_ = sock.Close()
		return fmt.Errorf("failed to subscribe: %w", err)
	}

	replaySocket, err := zmq.NewSocket(zmq.DEALER)
	if err != nil {
		sock.Close()
		return fmt.Errorf("failed to create DEALER socket: %w", err)
	}

	c.subSocket = sock
	c.replaySocket = replaySocket
	c.connected = true

	slog.Info("Successfully connected to vLLM pod %s at %s", c.config.PodKey, c.config.PodIP)

	return nil
}

// consume reads and processes messages from the SUB socket.
func (c *StaticZMQClient) consume() error {
	c.mu.RLock()
	socket := c.subSocket
	c.mu.RUnlock()

	if socket == nil {
		return fmt.Errorf("socket is nil")
	}

	poller := zmq.NewPoller()
	poller.Add(socket, zmq.POLLIN)

	// Poll for data
	polled, err := poller.Poll(c.config.PollTimeout)
	if err != nil {
		return fmt.Errorf("poll error: %w", err)
	}
	if len(polled) == 0 {
		return nil // No data, continue loop
	}

	// Process message
	if err := c.processMessage(socket); err != nil {
		return fmt.Errorf("failed to process message: %w", err)
	}

	return nil

}

func (c *StaticZMQClient) processMessage(socket *zmq.Socket) error {

	if socket == nil {
		return fmt.Errorf("socket is nil")
	}

	// Read Frames: [Topic, Seq, Payload]
	topic, err := socket.RecvBytes(0)
	if err != nil {
		return err
	}
	seqBytes, err := socket.RecvBytes(0)
	if err != nil {
		return err
	}
	payload, err := socket.RecvBytes(0)
	if err != nil {
		return err
	}

	// Validate Sequence
	if len(seqBytes) != 8 {
		return fmt.Errorf("invalid sequence length")
	}
	seq := int64(binary.BigEndian.Uint64(seqBytes))

	// Check Gap
	c.mu.RLock()
	lastSeq := c.lastSeq
	c.mu.RUnlock()

	if lastSeq != -1 && seq > lastSeq+1 {
		slog.Warn("Event gap detected",
			"service", c.config.PodKey,
			"missed", seq-lastSeq-1,
			"last", lastSeq,
			"current", seq,
		)
		// Trigger replay for missed events?
		// Usually we just log warning here, or could auto-trigger requestReplay
	}

	// Update Sequence immediately to keep state fresh
	c.mu.Lock()
	c.lastSeq = seq
	c.mu.Unlock()

	// Decode & Handle
	batch, err := DecodeEventBatch(payload)
	if err != nil {
		return fmt.Errorf("decode failed: %w", err)
	}

	slog.Info("Get batch!!!!!!!!")

	for _, event := range batch.Events {
		// Inject Source Name
		switch e := event.(type) {
		case *BlockStoredEvent:
			e.PodName = c.config.PodKey
		case *BlockRemovedEvent:
			e.PodName = c.config.PodKey
		}

		if err := c.eventHandler.HandleEvent(event); err != nil {
			slog.Error("Handler error", "service", c.config.PodKey, "error", err)
		}
	}

	slog.Debug("Processed batch", "service", c.config.PodKey, "seq", seq, "topic", string(topic))
	return nil

}

func (c *StaticZMQClient) requestReplay(fromSeq int64) error {
	c.mu.RLock()
	socket := c.replaySocket
	c.mu.RUnlock()

	if socket == nil {
		return fmt.Errorf("replay socket is nil")
	}

	req := make([]byte, 8)
	binary.BigEndian.PutUint64(req, uint64(fromSeq))

	if _, err := socket.SendBytes(req, 0); err != nil {
		return fmt.Errorf("failed to send replay request: %w", err)
	}

	// Ideally, we should wait for an ACK here if the protocol supports it
	// For simplicity in static client, we fire and forget the request,
	// assuming the server will send the replayed events via the SUB channel (or DEALER response)
	// Original code read response from DEALER, let's keep that.

	_ = socket.SetRcvtimeo(c.config.ReplayTimeout)

	resp, err := socket.RecvBytes(0)
	if err != nil {
		return fmt.Errorf("failed to receive replay response: %w", err)
	}

	slog.Info("Replay requested", "service", c.config.PodKey, "from", fromSeq, "resp_len", len(resp))
	return nil
}

func (c *StaticZMQClient) cleanupSockets() {
	if c.subSocket != nil {
		c.subSocket.Close()
		c.subSocket = nil
	}
	if c.replaySocket != nil {
		c.replaySocket.Close()
		c.replaySocket = nil
	}
	c.connected = false
}

func (c *StaticZMQClient) markDisconnected() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.connected = false
}

func (c *StaticZMQClient) isConnected() bool {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.connected
}

func (c *StaticZMQClient) getLastSequence() int64 {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.lastSeq
}
