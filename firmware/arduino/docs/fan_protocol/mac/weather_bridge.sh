#!/bin/zsh
# Garage fan weather bridge: Open-Meteo -> MQTT home/outdoor/* every 5 min.
export PATH=/opt/homebrew/bin:/usr/bin:/bin
# Replaces the defunct bridge that left a stale retained snapshot.
LAT=***REDACTED-LAT***
LON=***REDACTED-LON***
BROKER=***REDACTED-BROKER-HOST***
MQUSER=mqtt-client
MQPASS=***REMOVED-MQTT-PASSWORD***
J=$(curl -s -m 20 "https://api.open-meteo.com/v1/forecast?latitude=$LAT&longitude=$LON&current=temperature_2m,relative_humidity_2m,wind_speed_10m,weather_code&temperature_unit=fahrenheit&wind_speed_unit=ms")
[ -z "$J" ] && exit 1
read -r TF RH WS WC <<< "$(echo "$J" | /usr/bin/python3 -c "
import json,sys
c=json.load(sys.stdin)['current']
print(round(c['temperature_2m'],1), round(c['relative_humidity_2m']), round(c['wind_speed_10m'],1), c['weather_code'])
")"
[ -z "$TF" ] && exit 1
TC=$(/usr/bin/python3 -c "print(round(($TF-32)*5/9,1))")
P() { mosquitto_pub -h $BROKER -u $MQUSER -P $MQPASS -r -t "home/outdoor/$1" -m "$2"; }
P temp_f "$TF"
P temp "$TC"
P rh "$RH"
P hum "$RH"
P wind_mps "$WS"
P condition_code "$WC"
P ts "$(date +%s)"
