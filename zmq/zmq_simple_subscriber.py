#!/usr/bin/env python3
"""
Simple ZMQ Subscriber Example
This script subscribes to messages from a publisher
"""
import zmq
import time

def main():
    print("=" * 60)
    print("ZMQ SIMPLE SUBSCRIBER")
    print("=" * 60)
    
    # Step 1: Create ZMQ context
    print("\n[Step 1] Creating ZMQ context...")
    context = zmq.Context()
    print("✓ Context created")
    
    # Step 2: Create SUB socket
    print("\n[Step 2] Creating SUB socket...")
    socket = context.socket(zmq.SUB)
    print("✓ SUB socket created")
    
    # Step 3: Connect to publisher
    address = "tcp://localhost:5555"
    print(f"\n[Step 3] Connecting to publisher at {address}...")
    socket.connect(address)
    print(f"✓ Connected to {address}")
    
    # Step 4: Subscribe to topics (empty string = subscribe to all)
    print("\n[Step 4] Setting subscription filter...")
    socket.setsockopt_string(zmq.SUBSCRIBE, "")  # Subscribe to ALL messages
    print("✓ Subscribed to ALL topics (filter: '')")
    print("  (You can filter by topic, e.g., 'Temperature' to only receive those)")
    
    # Step 5: Receive messages
    print("\n[Step 5] Waiting for messages (press Ctrl+C to stop)...")
    print("-" * 60)
    
    try:
        message_count = 0
        while True:
            # Receive message (this blocks until a message arrives)
            message = socket.recv_string()
            message_count += 1
            
            # Log
            timestamp = time.strftime("%H:%M:%S")
            print(f"[{timestamp}] Received message #{message_count}: {message}")
            
    except KeyboardInterrupt:
        print("\n" + "-" * 60)
        print(f"\n[Shutdown] Received interrupt signal")
        print(f"  Total messages received: {message_count}")
    
    finally:
        # Step 6: Cleanup
        print("\n[Step 6] Cleaning up...")
        socket.close()
        print("✓ Socket closed")
        context.term()
        print("✓ Context terminated")
        print("\n" + "=" * 60)
        print("Subscriber stopped successfully")
        print("=" * 60)

if __name__ == "__main__":
    main()

