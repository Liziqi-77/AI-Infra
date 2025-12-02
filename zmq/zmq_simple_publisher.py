#!/usr/bin/env python3
"""
Simple ZMQ Publisher Example
This script publishes messages to subscribers
"""
import zmq
import time
import random

def main():
    print("=" * 60)
    print("ZMQ SIMPLE PUBLISHER")
    print("=" * 60)
    
    # Step 1: Create ZMQ context
    print("\n[Step 1] Creating ZMQ context...")
    context = zmq.Context()
    print("✓ Context created")
    
    # Step 2: Create PUB socket
    print("\n[Step 2] Creating PUB socket...")
    socket = context.socket(zmq.PUB)
    print("✓ PUB socket created")
    
    # Step 3: Bind to address
    address = "tcp://*:5555"
    print(f"\n[Step 3] Binding to {address}...")
    socket.bind(address)
    print(f"✓ Publisher bound to {address}")
    print("  (Listening on port 5555, accepting connections from subscribers)")
    
    # Step 4: Wait a moment for subscribers to connect
    print("\n[Step 4] Waiting 2 seconds for subscribers to connect...")
    time.sleep(2)
    print("✓ Ready to publish!")
    
    # Step 5: Publish messages
    print("\n[Step 5] Publishing messages (press Ctrl+C to stop)...")
    print("-" * 60)
    
    try:
        message_count = 0
        while True:
            # Create a simple message
            temperature = random.randint(15, 30)
            message = f"Temperature: {temperature}°C"
            
            # Send message
            socket.send_string(message)
            message_count += 1
            
            # Log
            timestamp = time.strftime("%H:%M:%S")
            print(f"[{timestamp}] Sent message #{message_count}: {message}")
            
            # Wait before sending next message
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\n" + "-" * 60)
        print(f"\n[Shutdown] Received interrupt signal")
        print(f"  Total messages sent: {message_count}")
    
    finally:
        # Step 6: Cleanup
        print("\n[Step 6] Cleaning up...")
        socket.close()
        print("✓ Socket closed")
        context.term()
        print("✓ Context terminated")
        print("\n" + "=" * 60)
        print("Publisher stopped successfully")
        print("=" * 60)

if __name__ == "__main__":
    main()

