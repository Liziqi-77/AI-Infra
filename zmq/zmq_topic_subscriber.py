#!/usr/bin/env python3
"""
ZMQ Subscriber with Topic Filter Example
This demonstrates how to subscribe to specific topics
"""
import zmq
import time
import sys

def main():
    print("=" * 60)
    print("ZMQ TOPIC SUBSCRIBER")
    print("=" * 60)
    
    # Get topic filter from command line (or default to all)
    if len(sys.argv) > 1:
        topic_filter = sys.argv[1]
    else:
        topic_filter = ""  # Subscribe to all
    
    context = zmq.Context()
    socket = context.socket(zmq.SUB)
    socket.connect("tcp://localhost:5556")
    
    # Set topic filter
    socket.setsockopt_string(zmq.SUBSCRIBE, topic_filter)
    
    if topic_filter == "":
        print("\n✓ Connected to publisher on port 5556")
        print("✓ Subscribed to ALL topics")
    else:
        print(f"\n✓ Connected to publisher on port 5556")
        print(f"✓ Subscribed to topic filter: '{topic_filter}'")
        print(f"  (Only messages starting with '{topic_filter}' will be received)")
    
    print("\nTIP: Run with argument to filter topics:")
    print("  python zmq_topic_subscriber.py Temperature")
    print("  python zmq_topic_subscriber.py Humidity")
    print("  python zmq_topic_subscriber.py Pressure")
    
    print("\nWaiting for messages...\n")
    print("-" * 60)
    
    try:
        message_count = 0
        while True:
            message = socket.recv_string()
            message_count += 1
            
            timestamp = time.strftime("%H:%M:%S")
            print(f"[{timestamp}] #{message_count:3d} {message}")
            
    except KeyboardInterrupt:
        print("\n" + "-" * 60)
        print(f"\n✓ Received {message_count} messages")
    
    finally:
        socket.close()
        context.term()
        print("✓ Subscriber stopped\n")

if __name__ == "__main__":
    main()

