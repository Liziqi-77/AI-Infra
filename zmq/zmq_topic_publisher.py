#!/usr/bin/env python3
"""
ZMQ Publisher with Topics Example
This demonstrates how to publish messages with different topics
"""
import zmq
import time
import random

def main():
    print("=" * 60)
    print("ZMQ TOPIC PUBLISHER")
    print("=" * 60)
    
    context = zmq.Context()
    socket = context.socket(zmq.PUB)
    socket.bind("tcp://*:5556")
    
    print("\n✓ Publisher started on port 5556")
    print("  Publishing messages with 3 topics:")
    print("    - Temperature")
    print("    - Humidity")
    print("    - Pressure")
    print("\nWaiting 2 seconds for subscribers...")
    time.sleep(2)
    print("Starting to publish...\n")
    print("-" * 60)
    
    try:
        message_count = 0
        while True:
            # Randomly choose a topic
            topics = ["Temperature", "Humidity", "Pressure"]
            topic = random.choice(topics)
            
            # Generate data based on topic
            if topic == "Temperature":
                value = random.randint(15, 30)
                unit = "°C"
            elif topic == "Humidity":
                value = random.randint(40, 80)
                unit = "%"
            else:  # Pressure
                value = random.randint(980, 1020)
                unit = "hPa"
            
            # Create message with topic prefix
            message = f"{topic} {value}{unit}"
            
            # Send message (topic is part of the message)
            socket.send_string(message)
            message_count += 1
            
            # Log with color/formatting
            timestamp = time.strftime("%H:%M:%S")
            topic_padded = topic.ljust(12)
            print(f"[{timestamp}] #{message_count:3d} [{topic_padded}] {value}{unit}")
            
            time.sleep(0.5)
            
    except KeyboardInterrupt:
        print("\n" + "-" * 60)
        print(f"\n✓ Published {message_count} messages total")
    
    finally:
        socket.close()
        context.term()
        print("✓ Publisher stopped\n")

if __name__ == "__main__":
    main()

