#!/usr/bin/env python3
import argparse
import os
import signal
import time

import paho.mqtt.client as mqtt


def main():
    ap = argparse.ArgumentParser(
        description=("Subscribe to headless publishes and optionally seed outdoor data")
    )
    ap.add_argument("--host", default=os.environ.get("MQTT_HOST", "localhost"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("MQTT_PORT", "1883")))
    ap.add_argument("--user", default=os.environ.get("MQTT_USER", ""))
    ap.add_argument("--password", default=os.environ.get("MQTT_PASS", ""))
    ap.add_argument(
        "--pub-base",
        default=os.environ.get("MQTT_PUB_BASE", "sensors/room"),
    )
    ap.add_argument(
        "--sub-base",
        default=os.environ.get("MQTT_SUB_BASE", "home/outdoor"),
    )
    ap.add_argument(
        "--seed",
        action="store_true",
        help=("Publish sample outdoor readings to SUB base"),
    )
    args = ap.parse_args()

    # Support paho-mqtt 1.x and 2.x by specifying callback API version when available
    if hasattr(mqtt, "CallbackAPIVersion"):
        try:
            c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION1)
        except TypeError:
            c = mqtt.Client()
    else:
        c = mqtt.Client()
    if args.user:
        c.username_pw_set(args.user, args.password or None)

    def on_connect(client, userdata, flags, rc):
        print(f"connected rc={rc}")
        # Subscribe to our inside and status topics
        client.subscribe(
            [
                (f"{args.pub_base}/inside/temp", 0),
                (f"{args.pub_base}/inside/hum", 0),
                (f"{args.pub_base}/status", 0),
                (f"{args.pub_base}/battery/voltage", 0),
                (f"{args.pub_base}/battery/percent", 0),
            ]
        )
        if args.seed:
            msgs = {
                f"{args.sub_base}/temp": "20.3",
                f"{args.sub_base}/hum": "55",
                f"{args.sub_base}/weather": "Cloudy",
                f"{args.sub_base}/wind_mps": "2.0",
                f"{args.sub_base}/pressure_hpa": "1013.2",
            }
            for t, p in msgs.items():
                client.publish(t, p, retain=True)
                print(f"seeded {t}={p}")

    def on_message(client, userdata, msg):
        print(f"{msg.topic}: {msg.payload.decode('utf-8', 'ignore')}")

    c.on_connect = on_connect
    c.on_message = on_message
    c.connect(args.host, args.port, 30)

    stop = False

    def handle_sigint(sig, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_sigint)

    while not stop:
        c.loop(timeout=1.0)
        time.sleep(0.1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
