# ESP32 Device Manager - Implementation Specification

> **Status: ✅ FULLY IMPLEMENTED** (Last updated: December 2024)

## Overview

Transform the existing web simulator into a comprehensive **Device Manager** that can:
1. ✅ Flash firmware to connected ESP32 devices
2. ✅ Display live serial console output
3. ✅ Simulate MQTT broker with Home Assistant-like data
4. ✅ Pull and display device screenshots
5. ✅ Control device settings (sleep interval, diagnostic mode)
6. ✅ Show real-time device status

## Quick Start

```bash
# Combined startup (recommended)
./scripts/run_device_manager.sh

# With frontend hot-reload for development
./scripts/run_device_manager.sh --dev

# Manual start
python scripts/start_device_manager.py --port 8080
```

See `web/manager/README.md` for detailed usage instructions.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Browser (Vite + React)                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Display  │ │ Serial   │ │  Flash   │ │  MQTT    │ │ Status   │  │
│  │ Viewer   │ │ Console  │ │ Manager  │ │ Inspector│ │ Dashboard│  │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘ └────┬─────┘  │
│       │            │            │            │            │         │
│       └────────────┴────────────┴─────┬──────┴────────────┘         │
│                                       │ WebSocket                   │
└───────────────────────────────────────┼─────────────────────────────┘
                                        │
┌───────────────────────────────────────┼─────────────────────────────┐
│                     Python Backend (FastAPI)                        │
│                                       │                             │
│  ┌────────────────────────────────────┴────────────────────────┐   │
│  │                    WebSocket Hub                             │   │
│  │  (broadcasts serial, mqtt, status to all connected clients)  │   │
│  └──────────────────────────────────────────────────────────────┘   │
│       │            │            │            │                      │
│  ┌────┴────┐ ┌─────┴─────┐ ┌────┴────┐ ┌─────┴─────┐               │
│  │ Serial  │ │   Flash   │ │  MQTT   │ │ Screenshot│               │
│  │ Manager │ │  Manager  │ │ Broker  │ │  Handler  │               │
│  └────┬────┘ └─────┬─────┘ └────┬────┘ └─────┬─────┘               │
│       │            │            │            │                      │
│   pyserial    subprocess     paho-mqtt   base64 decode              │
│              (esptool.py)   (embedded)                              │
└───────────────────────────────────────────────────────────────────────┘
```

---

## File Structure (Actual Implementation)

```
web/
├── manager/                        # ✅ Device Manager frontend
│   ├── src/
│   │   ├── App.jsx                 # Main app with tab navigation
│   │   ├── main.jsx                # React entry point
│   │   ├── components/
│   │   │   ├── DisplayViewer.jsx   # Screenshot display + refresh
│   │   │   ├── SerialConsole.jsx   # Live serial output
│   │   │   ├── FlashManager.jsx    # Flash controls + progress
│   │   │   ├── MqttInspector.jsx   # MQTT message viewer
│   │   │   ├── StatusDashboard.jsx # Device status cards
│   │   │   ├── ControlPanel.jsx    # Sleep interval, reboot, etc.
│   │   │   └── DeviceSelector.jsx  # Port/device dropdown
│   │   ├── hooks/
│   │   │   └── useWebSocket.js     # WebSocket connection hook
│   │   ├── api/
│   │   │   └── deviceApi.js        # REST API calls
│   │   └── styles/
│   │       └── manager.css         # Manager-specific styles
│   ├── index.html
│   ├── package.json
│   ├── vite.config.js
│   └── README.md                   # ✅ Usage documentation
│
├── sim/                            # Original simulator (preserved)
│   └── ...
│
scripts/
├── device_manager/                 # ✅ Backend modules
│   ├── __init__.py
│   ├── server.py                   # FastAPI main server (500+ lines)
│   ├── serial_manager.py           # Serial port handling
│   ├── flash_manager.py            # PlatformIO wrapper
│   ├── mqtt_broker.py              # Paho MQTT client (connects to mosquitto)
│   ├── mqtt_simulator.py           # Fake HA data generator
│   ├── screenshot_handler.py       # Screenshot decode/store
│   ├── websocket_hub.py            # WebSocket broadcast manager
│   └── config.py                   # Server configuration
├── start_device_manager.py         # ✅ Entry point script
└── run_device_manager.sh           # ✅ Combined startup script
```

### Implementation Notes

- **Frontend location**: `web/manager/` (separate from simulator in `web/sim/`)
- **MQTT approach**: Uses paho-mqtt client connecting to external Mosquitto broker
  (more reliable than embedded Python MQTT broker)
- **Flash tool**: Uses PlatformIO CLI (`pio run -t upload`) instead of raw esptool

---

## Backend Implementation

### 1. Server Entry Point (`scripts/start_device_manager.py`)

```python
#!/usr/bin/env python3
"""
ESP32 Device Manager - Start the backend server
Usage: python scripts/start_device_manager.py [--port 8080] [--no-broker]
"""
import argparse
import uvicorn

def main():
    parser = argparse.ArgumentParser(description='ESP32 Device Manager')
    parser.add_argument('--port', type=int, default=8080, help='Server port')
    parser.add_argument('--no-broker', action='store_true', help='Disable embedded MQTT broker')
    parser.add_argument('--serial-port', type=str, default=None, help='Serial port (auto-detect if not specified)')
    args = parser.parse_args()
    
    # Start FastAPI server
    uvicorn.run(
        "device_manager.server:app",
        host="0.0.0.0",
        port=args.port,
        reload=False
    )

if __name__ == "__main__":
    main()
```

---

### 2. Main Server (`scripts/device_manager/server.py`)

**Endpoints to implement:**

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/ports` | List available serial ports |
| POST | `/api/serial/connect` | Connect to serial port |
| POST | `/api/serial/disconnect` | Disconnect from serial |
| POST | `/api/serial/send` | Send command to serial |
| GET | `/api/serial/status` | Get serial connection status |
| POST | `/api/flash/start` | Start flash process |
| GET | `/api/flash/status` | Get flash progress |
| POST | `/api/flash/cancel` | Cancel flash in progress |
| GET | `/api/mqtt/status` | Get MQTT broker status |
| POST | `/api/mqtt/publish` | Publish MQTT message |
| GET | `/api/mqtt/subscriptions` | List active subscriptions |
| POST | `/api/device/screenshot` | Request screenshot from device |
| GET | `/api/device/screenshot/latest` | Get latest screenshot (base64 PNG) |
| POST | `/api/device/command` | Send command (reboot, diag_mode, etc.) |
| GET | `/api/device/status` | Get device status (battery, heap, etc.) |
| POST | `/api/config/sleep-interval` | Set device sleep interval |
| WS | `/ws` | WebSocket for live updates |

**WebSocket message types:**

```typescript
// Server -> Client
{ type: "serial", data: "log line here", timestamp: 1234567890 }
{ type: "mqtt", topic: "espsensor/...", payload: "72.3", direction: "in"|"out" }
{ type: "flash_progress", percent: 45, stage: "writing", message: "..." }
{ type: "flash_complete", success: true, message: "..." }
{ type: "screenshot", data: "base64...", width: 250, height: 122 }
{ type: "device_status", battery: 87, heap: 45000, connected: true }
{ type: "error", source: "serial"|"mqtt"|"flash", message: "..." }

// Client -> Server
{ type: "serial_send", data: "status\n" }
{ type: "mqtt_publish", topic: "...", payload: "..." }
{ type: "request_screenshot" }
```

---

### 3. Serial Manager (`scripts/device_manager/serial_manager.py`)

**Responsibilities:**
- Auto-detect ESP32 serial ports (look for CP210x, CH340, FTDI)
- Connect/disconnect with configurable baud rate (default 115200)
- Read serial data in background thread
- Broadcast lines to WebSocket hub
- Send commands to device
- Parse structured output (JSON status messages if device sends them)

**Key implementation details:**

```python
class SerialManager:
    def __init__(self, websocket_hub):
        self.port = None
        self.serial = None
        self.reader_thread = None
        self.hub = websocket_hub
    
    def list_ports(self) -> List[dict]:
        """Return list of available ports with metadata"""
        # Use serial.tools.list_ports
        # Filter for likely ESP32 devices
        # Return: [{"port": "/dev/ttyUSB0", "description": "CP2104", "hwid": "..."}]
    
    def connect(self, port: str, baud: int = 115200) -> bool:
        """Connect to specified port"""
        # Open serial connection
        # Start reader thread
        # Return success/failure
    
    def disconnect(self):
        """Disconnect and cleanup"""
    
    def send(self, data: str):
        """Send data to device"""
    
    def _reader_loop(self):
        """Background thread to read serial and broadcast"""
        # Read lines
        # Parse for special markers (JSON status, screenshot data)
        # Broadcast to websocket hub
```

---

### 4. Flash Manager (`scripts/device_manager/flash_manager.py`)

**Responsibilities:**
- Wrap esptool.py or platformio CLI
- Stream progress to WebSocket
- Support cancellation
- Handle different build configurations

**Key implementation details:**

```python
class FlashManager:
    def __init__(self, websocket_hub):
        self.hub = websocket_hub
        self.process = None
        self.is_flashing = False
    
    def flash(self, port: str, firmware_path: str = None, build_config: str = "dev") -> bool:
        """
        Flash firmware to device.
        
        If firmware_path is None, build from source first using platformio.
        build_config: "dev" (DEV_NO_SLEEP), "prod", "battery_test"
        """
        # 1. If no firmware_path, run: pio run -e <config>
        # 2. Run esptool.py with progress parsing
        # 3. Stream progress via websocket
        # 4. Return success/failure
    
    def _run_with_progress(self, cmd: List[str]):
        """Run command and parse/broadcast progress"""
        # Use subprocess.Popen with stdout pipe
        # Parse esptool output for progress percentages
        # Broadcast: { type: "flash_progress", percent: X, stage: "erasing"|"writing"|"verifying" }
    
    def cancel(self):
        """Cancel flash in progress"""
        # Kill subprocess if running
```

**Build configurations to support:**

| Config | Description | PlatformIO Env |
|--------|-------------|----------------|
| `dev` | DEV_NO_SLEEP=1, BOOT_DEBUG=1 | `env:feather_dev` |
| `prod` | Normal operation with sleep | `env:feather` |
| `battery_test` | Verbose battery logging | `env:feather_battery` |

---

### 5. MQTT Broker (`scripts/device_manager/mqtt_broker.py`)

**Responsibilities:**
- Run embedded MQTT broker (use `amqtt` or `hbmqtt` Python library)
- Allow device to connect without external broker
- Capture all messages for inspector
- Support message injection

**Key implementation details:**

```python
class EmbeddedMqttBroker:
    def __init__(self, websocket_hub, port: int = 1883):
        self.hub = websocket_hub
        self.port = port
        self.broker = None
        self.message_log = []  # Last N messages
    
    async def start(self):
        """Start the embedded broker"""
        # Use amqtt or hbmqtt
        # Configure to allow anonymous connections
        # Hook message handler to capture all messages
    
    def on_message(self, topic: str, payload: bytes, direction: str):
        """Called for every message through broker"""
        # Log message
        # Broadcast to websocket: { type: "mqtt", topic, payload, direction }
    
    def publish(self, topic: str, payload: str, retain: bool = False):
        """Publish a message (for injection/simulation)"""
```

---

### 6. MQTT Simulator (`scripts/device_manager/mqtt_simulator.py`)

**Responsibilities:**
- Generate fake Home Assistant-style data
- Simulate outside weather conditions
- Configurable patterns (sine wave temp, random weather, etc.)

**Key implementation details:**

```python
class MqttSimulator:
    def __init__(self, broker: EmbeddedMqttBroker):
        self.broker = broker
        self.running = False
        self.config = {
            "temp_min": 40,
            "temp_max": 85,
            "temp_period_hours": 24,  # Full sine wave cycle
            "weather_options": ["sunny", "partly cloudy", "cloudy", "rain", "snow"],
            "update_interval_sec": 60
        }
    
    def start(self):
        """Start publishing simulated data"""
        # Background task to publish every update_interval_sec
    
    def stop(self):
        """Stop simulation"""
    
    def _generate_temperature(self) -> float:
        """Generate temperature based on time of day (sine wave)"""
        # Peak at 3pm, low at 3am
        # Add small random noise
    
    def _generate_weather(self) -> str:
        """Generate weather condition"""
        # Weighted random, or follow a pattern
    
    def _publish_cycle(self):
        """Publish one cycle of simulated data"""
        # Publish to: homeassistant/sensor/outside/temp_f
        # Publish to: homeassistant/sensor/outside/condition
        # Publish to: homeassistant/sensor/outside/humidity
```

**MQTT Topics to simulate (matching firmware expectations):**

| Topic | Example Payload | Notes |
|-------|-----------------|-------|
| `{MQTT_SUB_BASE}/temp_f` | `72.5` | Outside temperature °F |
| `{MQTT_SUB_BASE}/temp` | `22.5` | Outside temperature °C (legacy) |
| `{MQTT_SUB_BASE}/pressure_hpa` | `1013.2` | Outside barometric pressure hPa |
| `{MQTT_SUB_BASE}/pressure` | `1013.2` | Outside barometric pressure hPa (legacy) |
| `{MQTT_SUB_BASE}/condition` | `sunny` | Weather condition text |
| `{MQTT_SUB_BASE}/humidity` | `45` | Outside humidity % |
| `{MQTT_SUB_BASE}/wind_mph` | `5.2` | Wind speed |

---

### 7. Screenshot Handler (`scripts/device_manager/screenshot_handler.py`)

**Responsibilities:**
- Request screenshot from device via MQTT command
- Receive base64 screenshot data from device
- Decode and store latest screenshot
- Convert 1-bit display buffer to PNG

**Key implementation details:**

```python
class ScreenshotHandler:
    def __init__(self, mqtt_broker, websocket_hub):
        self.broker = mqtt_broker
        self.hub = websocket_hub
        self.latest_screenshot = None
        self.latest_metadata = None
        
        # Subscribe to screenshot topics
        # espsensor/+/debug/screenshot/meta
        # espsensor/+/debug/screenshot/data
    
    def request_screenshot(self, device_id: str):
        """Request screenshot from device"""
        # Publish to: espsensor/{device_id}/cmd/screenshot
        # Payload: "capture"
    
    def on_screenshot_meta(self, device_id: str, meta: dict):
        """Handle incoming metadata"""
        # meta: { width, height, format, data_size }
    
    def on_screenshot_data(self, device_id: str, data: str):
        """Handle incoming base64 data"""
        # Decode base64
        # Convert 1-bit buffer to PNG
        # Store and broadcast
    
    def _convert_1bit_to_png(self, data: bytes, width: int, height: int) -> bytes:
        """Convert 1-bit packed display buffer to PNG"""
        # Use PIL/Pillow
        # 1 bit per pixel, MSB first
        # Return PNG bytes
```

---

### 8. WebSocket Hub (`scripts/device_manager/websocket_hub.py`)

**Responsibilities:**
- Manage connected WebSocket clients
- Broadcast messages to all clients
- Handle client-to-server messages

```python
class WebSocketHub:
    def __init__(self):
        self.clients: Set[WebSocket] = set()
    
    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.clients.add(websocket)
    
    def disconnect(self, websocket: WebSocket):
        self.clients.discard(websocket)
    
    async def broadcast(self, message: dict):
        """Send message to all connected clients"""
        data = json.dumps(message)
        for client in self.clients:
            try:
                await client.send_text(data)
            except:
                self.disconnect(client)
    
    async def handle_message(self, websocket: WebSocket, message: dict):
        """Handle incoming message from client"""
        # Route based on message type
```

---

## Frontend Implementation

### 1. App Layout (`web/src/App.jsx`)

**Structure:**
```jsx
<div className="device-manager">
  <header>
    <h1>ESP32 Device Manager</h1>
    <DeviceSelector />  {/* Port dropdown + connect button */}
  </header>
  
  <main>
    <Tabs>
      <Tab label="Dashboard">
        <StatusDashboard />
        <ControlPanel />
      </Tab>
      <Tab label="Display">
        <DisplayViewer />
      </Tab>
      <Tab label="Console">
        <SerialConsole />
      </Tab>
      <Tab label="Flash">
        <FlashManager />
      </Tab>
      <Tab label="MQTT">
        <MqttInspector />
      </Tab>
    </Tabs>
  </main>
</div>
```

---

### 2. WebSocket Hook (`web/src/hooks/useWebSocket.js`)

```javascript
export function useWebSocket(url) {
  const [connected, setConnected] = useState(false);
  const [messages, setMessages] = useState([]);
  const wsRef = useRef(null);
  
  useEffect(() => {
    const ws = new WebSocket(url);
    
    ws.onopen = () => setConnected(true);
    ws.onclose = () => setConnected(false);
    ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      setMessages(prev => [...prev.slice(-1000), msg]); // Keep last 1000
    };
    
    wsRef.current = ws;
    return () => ws.close();
  }, [url]);
  
  const send = (message) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(message));
    }
  };
  
  return { connected, messages, send };
}
```

---

### 3. Serial Console Component (`web/src/components/SerialConsole.jsx`)

**Features:**
- Auto-scrolling log view
- Line coloring based on log level ([ERROR] = red, [WARN] = yellow)
- Filter input to search/filter lines
- Command input to send to device
- Clear button
- Pause/resume auto-scroll
- Copy button

**Props/State:**
```javascript
// Filter serial messages from WebSocket
const serialMessages = messages.filter(m => m.type === 'serial');

// State
const [filter, setFilter] = useState('');
const [paused, setPaused] = useState(false);
const [commandInput, setCommandInput] = useState('');
```

---

### 4. Flash Manager Component (`web/src/components/FlashManager.jsx`)

**Features:**
- Build config selector (dev/prod/battery_test)
- Flash button with confirmation
- Progress bar during flash
- Log output from flash process
- Cancel button during flash

**API calls:**
```javascript
// Start flash
await fetch('/api/flash/start', {
  method: 'POST',
  body: JSON.stringify({ port, config: 'dev' })
});

// Progress comes via WebSocket: { type: "flash_progress", percent, stage, message }
```

---

### 5. Display Viewer Component (`web/src/components/DisplayViewer.jsx`)

**Features:**
- Display screenshot image (scaled 2x or 3x for visibility)
- "Refresh" button to request new screenshot
- Auto-refresh toggle with interval selector
- Side-by-side with simulator render (optional)
- Timestamp of last screenshot

**Layout:**
```
┌─────────────────────────────────────────┐
│  [Refresh] [Auto: ▼ Off/5s/10s/30s]    │
├─────────────────────────────────────────┤
│                                         │
│     ┌───────────────────────┐           │
│     │      72°F             │           │
│     │      OFFICE           │           │
│     │      45% RH           │           │
│     └───────────────────────┘           │
│                                         │
│     Last updated: 12:34:56              │
└─────────────────────────────────────────┘
```

---

### 6. MQTT Inspector Component (`web/src/components/MqttInspector.jsx`)

**Features:**
- Message log (topic, payload, direction, timestamp)
- Direction indicators (→ outgoing, ← incoming)
- Topic filter
- Payload search
- Manual publish form (topic + payload inputs)
- Simulation controls (start/stop fake data)

**Message display:**
```
12:34:56 → espsensor/office/inside/temperature  72.3
12:34:56 → espsensor/office/battery/percent     87
12:34:57 ← homeassistant/sensor/outside/temp_f  58.0
```

---

### 7. Control Panel Component (`web/src/components/ControlPanel.jsx`)

**Features:**
- Sleep interval slider (1min to 60min)
- Reboot button
- Diagnostic mode toggle
- Force full refresh button

**API calls:**
```javascript
// Set sleep interval
await fetch('/api/config/sleep-interval', {
  method: 'POST',
  body: JSON.stringify({ interval_sec: 300 })
});

// Send command
await fetch('/api/device/command', {
  method: 'POST',
  body: JSON.stringify({ command: 'reboot' })
});
```

---

### 8. Status Dashboard Component (`web/src/components/StatusDashboard.jsx`)

**Features:**
- Connection status cards (Serial, MQTT, WiFi)
- Battery status (voltage, percent, estimated days)
- Memory stats (heap, fragmentation)
- Last wake info
- Error counts

**Layout:**
```
┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
│ SERIAL   │ │  MQTT    │ │ BATTERY  │ │  MEMORY  │
│   ✅     │ │   ✅     │ │   87%    │ │  45KB    │
│ ttyUSB0  │ │ 12 msgs  │ │  4.1V    │ │  12% frag│
└──────────┘ └──────────┘ └──────────┘ └──────────┘
```

---

## Dependencies

### Python (`requirements-manager.txt`)
```
fastapi>=0.100.0
uvicorn>=0.23.0
websockets>=11.0
pyserial>=3.5
paho-mqtt>=1.6.0
amqtt>=0.11.0  # Embedded MQTT broker
Pillow>=10.0.0  # Screenshot conversion
esptool>=4.6.0
```

### Frontend (add to existing `package.json`)
```json
{
  "dependencies": {
    "react-tabs": "^6.0.0"  // Or use existing tab solution
  }
}
```

---

## Configuration

### Server config (`scripts/device_manager/config.py`)

```python
from dataclasses import dataclass

@dataclass
class ManagerConfig:
    # Server
    host: str = "0.0.0.0"
    port: int = 8080
    
    # Serial
    default_baud: int = 115200
    serial_timeout: float = 1.0
    
    # MQTT
    mqtt_broker_port: int = 1883
    mqtt_broker_enabled: bool = True
    
    # Simulator
    simulator_enabled: bool = True
    simulator_interval_sec: int = 60
    mqtt_sub_base: str = "homeassistant/sensor/outside"
    
    # Flash
    platformio_env_dev: str = "feather_dev"
    platformio_env_prod: str = "feather"
    firmware_dir: str = ".pio/build"
    
    # Screenshot
    display_width: int = 250
    display_height: int = 122
```

---

## Implementation Order

### Phase 1: Foundation ✅ COMPLETE
1. [x] Create `scripts/device_manager/` directory structure
2. [x] Implement `server.py` with basic FastAPI app
3. [x] Implement `websocket_hub.py`
4. [x] Implement `serial_manager.py` (list ports, connect, read)
5. [x] Create basic frontend with serial console only
6. [x] Test serial communication end-to-end

### Phase 2: Flash Support ✅ COMPLETE
1. [x] Implement `flash_manager.py`
2. [x] Add flash progress parsing
3. [x] Create `FlashManager.jsx` component
4. [x] Test flash workflow end-to-end

### Phase 3: MQTT ✅ COMPLETE
1. [x] Implement `mqtt_broker.py` (paho-mqtt client)
2. [x] Implement `mqtt_simulator.py` (fake data)
3. [x] Create `MqttInspector.jsx` component
4. [x] Test device connecting to broker

### Phase 4: Screenshots & Control ✅ COMPLETE
1. [x] Implement `screenshot_handler.py`
2. [x] Create `DisplayViewer.jsx` component
3. [x] Create `ControlPanel.jsx` component
4. [x] Create `StatusDashboard.jsx` component
5. [x] Test screenshot capture end-to-end

### Phase 5: Polish ✅ COMPLETE
1. [x] Add error handling throughout
2. [x] Add loading states to UI
3. [x] Add configuration support
4. [x] Write startup script with CLI args (`start_device_manager.py`)
5. [x] Create combined startup script (`run_device_manager.sh`)
6. [x] Update documentation (`web/manager/README.md`)

### Phase 6: Firmware Integration ✅ COMPLETE (Added)
1. [x] Add `sleep_interval` MQTT command handler
2. [x] Add `reboot` MQTT command handler
3. [x] Add `screenshot` MQTT command handler
4. [x] Update backend to send proper MQTT commands

---

## Testing Checklist

- [x] Serial: Can list available ports
- [x] Serial: Can connect to device
- [x] Serial: Live output appears in console
- [x] Serial: Can send commands to device
- [x] Flash: Can flash dev firmware
- [x] Flash: Progress bar updates
- [x] Flash: Can cancel flash
- [x] MQTT: Broker client connects to Mosquitto
- [x] MQTT: Device connects to broker
- [x] MQTT: Messages appear in inspector
- [x] MQTT: Can inject messages
- [x] MQTT: Simulator generates fake data
- [x] Screenshot: Can request screenshot
- [x] Screenshot: Screenshot displays correctly
- [x] Control: Can change sleep interval
- [x] Control: Can reboot device
- [x] Status: Shows correct battery level
- [x] Status: Shows correct memory stats

---

## Implementation Notes (Post-Implementation)

1. **Architecture decisions**:
   - Frontend lives in `web/manager/` separate from simulator (`web/sim/`)
   - Uses paho-mqtt client connecting to external Mosquitto (more reliable than Python embedded broker)
   - Uses PlatformIO for building/flashing (not raw esptool)

2. **Error handling**: Implemented throughout with try/catch and user-visible error messages

3. **Threading**: Serial reading uses background thread, rest uses asyncio

4. **State management**: Uses React hooks (useState, useEffect) and WebSocket for real-time updates

5. **Firmware commands added**:
   - `sleep_interval` - Set custom wake interval (60-3600 seconds)
   - `reboot` - Remote device restart
   - `screenshot` - Capture display to MQTT
   - `diagnostic_mode` - Toggle verbose logging

6. **Screenshot format**: 1-bit packed buffer, 250x122 pixels, converted to PNG via PIL

---

## Design Decisions Made

| Question | Decision |
|----------|----------|
| Multiple devices? | Single device at a time (via device ID selector) |
| MQTT persistence? | No - messages cleared on page refresh |
| Authentication? | No - local development tool |
| Editable flash configs? | Presets only (dev, prod, battery_test) |

---

## Future Enhancements (Not Implemented)

- [ ] Multi-device simultaneous management
- [ ] MQTT message persistence/history
- [ ] Build config editor in UI
- [ ] OTA firmware update support
- [x] ~~Device auto-discovery via mDNS~~ ✅ IMPLEMENTED

### mDNS Device Discovery (Added Dec 2024)

Devices are automatically discovered on the local network via mDNS:
- Firmware advertises `_espsensor._tcp` service with version and room info
- Backend uses `zeroconf` library to scan for devices
- **Safety feature**: Only ONE device can be "targeted" at a time
- Confirmation dialog required when switching targets
- All commands go to the targeted device only
