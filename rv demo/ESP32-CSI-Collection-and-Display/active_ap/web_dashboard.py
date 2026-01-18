#!/usr/bin/env python3
"""
ESP32 CSI Motion Detection - Lightweight Web Dashboard

Receives simple JSON status from ESP32:
{"motion":1,"rssi":-45,"pps":100,"pkts":12345}

No raw CSI data - all processing done on-device!
"""

import json
import socket
import threading
import time
from collections import deque
from http.server import HTTPServer, SimpleHTTPRequestHandler

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False
    print("[WARN] pyserial not installed - restart button will not work")
    print("       Install with: pip install pyserial")

# Configuration
UDP_HOST = "192.168.4.2"  # PC's IP on ESP32-AP network
UDP_PORT = 8848
WEB_PORT = 8080

# ESP32 AP address for sending commands
ESP32_IP = "192.168.4.1"
ESP32_CMD_PORT = 8849


def send_command_to_esp32(command):
    """Send a tuning command to the ESP32 via UDP."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(command.encode(), (ESP32_IP, ESP32_CMD_PORT))
        sock.close()
        print(f"[CMD] Sent: {command}")
        return True
    except Exception as e:
        print(f"[CMD] Failed to send: {e}")
        return False

# ESP32 device ports (AP and clients)
DEVICE_PORTS = ["COM19", "COM15", "COM17"]  # AP first, then clients

# Shared state
def create_fresh_status():
    """Create a fresh status dictionary with empty history."""
    return {
        "motion": False,
        "rssi": 0,
        "pps": 0,
        "packets": 0,
        "last_update": 0,
        "connected": False,
        "calibrated": False,
        "threshold": 0,
        "variance": 0,
        "motion_history": deque(maxlen=100),
        "rssi_history": deque(maxlen=100),
        "pps_history": deque(maxlen=100),
    }

status = create_fresh_status()
status_lock = threading.Lock()


def reset_device(port):
    """Reset an ESP32 device by toggling DTR on its serial port."""
    if not SERIAL_AVAILABLE:
        return False, "pyserial not installed"
    
    try:
        ser = serial.Serial(port, 115200, timeout=0.1)
        ser.setDTR(False)
        time.sleep(0.1)
        ser.setDTR(True)
        time.sleep(0.1)
        ser.close()
        return True, "OK"
    except serial.SerialException as e:
        return False, str(e)
    except Exception as e:
        return False, str(e)


def restart_all_devices():
    """Reset all ESP32 devices in sequence with proper timing."""
    results = {}
    
    print("[RESTART] Phase 1: Resetting AP (COM19)...")
    success, msg = reset_device(DEVICE_PORTS[0])  # AP first
    results[DEVICE_PORTS[0]] = {"success": success, "message": msg}
    print(f"[RESTART] {DEVICE_PORTS[0]}: {'OK' if success else 'FAILED - ' + msg}")
    
    # Wait for AP to initialize its WiFi network
    print("[RESTART] Waiting 3s for AP WiFi network to come up...")
    time.sleep(3.0)
    
    # Reset clients
    print("[RESTART] Phase 2: Resetting clients...")
    for port in DEVICE_PORTS[1:]:
        success, msg = reset_device(port)
        results[port] = {"success": success, "message": msg}
        print(f"[RESTART] {port}: {'OK' if success else 'FAILED - ' + msg}")
        time.sleep(0.3)
    
    # Wait for clients to connect and calibration to begin
    print("[RESTART] Waiting 2s for clients to connect...")
    time.sleep(2.0)
    
    print("[RESTART] Complete! Calibration will take ~2-3 seconds. Keep area STILL!")
    return results


def udp_receiver():
    """Receive status updates from ESP32"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((UDP_HOST, UDP_PORT))
    sock.settimeout(2.0)
    
    print(f"[UDP] Listening on {UDP_HOST}:{UDP_PORT}")
    
    while True:
        try:
            data, addr = sock.recvfrom(512)
            try:
                msg = json.loads(data.decode('utf-8'))
                with status_lock:
                    status["motion"] = msg.get("motion", 0) == 1
                    status["rssi"] = msg.get("rssi", 0)
                    status["pps"] = msg.get("pps", 0)
                    status["packets"] = msg.get("pkts", 0)
                    status["calibrated"] = msg.get("cal", 0) == 1
                    status["threshold"] = msg.get("thresh", 0)
                    status["variance"] = msg.get("var", 0)
                    status["last_update"] = time.time()
                    status["connected"] = True
                    
                    # Add to history
                    status["motion_history"].append(1 if status["motion"] else 0)
                    status["rssi_history"].append(status["rssi"])
                    status["pps_history"].append(status["pps"])
                    
            except json.JSONDecodeError:
                pass
                
        except socket.timeout:
            with status_lock:
                if time.time() - status["last_update"] > 3:
                    if status["connected"]:
                        print("[UDP] Device disconnected - clearing stale data")
                        # Clear history when device disconnects
                        status["motion_history"].clear()
                        status["rssi_history"].clear()
                        status["pps_history"].clear()
                        status["motion"] = False
                        status["pps"] = 0
                        status["calibrated"] = False
                    status["connected"] = False
        except Exception as e:
            print(f"[UDP] Error: {e}")


HTML_PAGE = """<!DOCTYPE html>
<html>
<head>
    <title>ESP32 CSI Motion Detection</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: 'Segoe UI', system-ui, sans-serif; 
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            color: #eee;
            min-height: 100vh;
        }
        .header {
            background: rgba(0,0,0,0.3);
            padding: 30px 20px;
            text-align: center;
            border-bottom: 1px solid rgba(0,217,255,0.3);
        }
        h1 { 
            color: #00d9ff; 
            font-size: 2.5em;
            font-weight: 300;
            letter-spacing: 2px;
        }
        .subtitle { color: #888; margin-top: 10px; }
        .container { 
            max-width: 900px; 
            margin: 0 auto; 
            padding: 30px 20px;
        }
        
        /* Big Motion Indicator */
        .motion-box {
            background: rgba(0,0,0,0.4);
            border-radius: 20px;
            padding: 60px 40px;
            text-align: center;
            margin-bottom: 30px;
            border: 2px solid rgba(255,255,255,0.1);
            transition: all 0.3s ease;
        }
        .motion-box.active {
            background: rgba(0, 255, 100, 0.2);
            border-color: #00ff64;
            box-shadow: 0 0 60px rgba(0, 255, 100, 0.4);
        }
        .motion-status {
            font-size: 4em;
            font-weight: 700;
            letter-spacing: 5px;
            transition: all 0.2s ease;
        }
        .motion-status.detected { color: #00ff64; }
        .motion-status.idle { color: #444; }
        .motion-label {
            font-size: 1.2em;
            color: #888;
            margin-top: 15px;
            text-transform: uppercase;
            letter-spacing: 3px;
        }
        
        /* Stats Grid */
        .stats {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 20px;
            margin-bottom: 30px;
        }
        .stat {
            background: rgba(0,0,0,0.3);
            padding: 25px;
            border-radius: 15px;
            text-align: center;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .stat-value {
            font-size: 2.5em;
            font-weight: 600;
            color: #00d9ff;
        }
        .stat-label {
            color: #666;
            font-size: 0.85em;
            margin-top: 8px;
            text-transform: uppercase;
            letter-spacing: 1px;
        }
        
        /* Chart */
        .chart-box {
            background: rgba(0,0,0,0.3);
            border-radius: 15px;
            padding: 25px;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .chart-title {
            color: #00d9ff;
            font-size: 1.1em;
            margin-bottom: 15px;
            font-weight: 400;
        }
        canvas {
            width: 100% !important;
            height: 120px !important;
            background: rgba(0,0,0,0.3);
            border-radius: 8px;
        }
        
        /* Connection Status */
        .connection {
            text-align: center;
            padding: 15px;
            font-size: 0.9em;
        }
        .connection.online { color: #00ff64; }
        .connection.offline { color: #ff4444; }
        
        .dot {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            margin-right: 8px;
            animation: pulse 1s infinite;
        }
        .dot.online { background: #00ff64; }
        .dot.offline { background: #ff4444; animation: none; }
        
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        
        @media (max-width: 600px) {
            .stats { grid-template-columns: 1fr; }
            .motion-status { font-size: 2.5em; }
            h1 { font-size: 1.8em; }
        }
    </style>
</head>
<body>
    <div class="header">
        <h1>MOTION DETECTION</h1>
        <p class="subtitle">ESP32 WiFi CSI Sensing</p>
    </div>
    
    <div class="container">
        <div class="motion-box" id="motionBox">
            <div class="motion-status idle" id="motionStatus">NO MOTION</div>
            <div class="motion-label">Detection Status</div>
        </div>
        
        <div class="stats">
            <div class="stat">
                <div class="stat-value" id="rssi">--</div>
                <div class="stat-label">Signal (dBm)</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="pps">--</div>
                <div class="stat-label">Packets/sec</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="total">--</div>
                <div class="stat-label">Total Packets</div>
            </div>
        </div>
        
        <div class="stats" style="margin-top: -10px;">
            <div class="stat">
                <div class="stat-value" id="variance" style="font-size: 1.5em;">--</div>
                <div class="stat-label">Variance</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="threshold" style="font-size: 1.5em;">--</div>
                <div class="stat-label">Threshold</div>
            </div>
            <div class="stat">
                <div class="stat-value" id="calStatus" style="font-size: 1.2em;">--</div>
                <div class="stat-label">Calibration</div>
            </div>
        </div>
        
        <div class="chart-box">
            <div class="chart-title">Motion Activity</div>
            <canvas id="chart"></canvas>
        </div>
        
        <!-- Tuning Controls -->
        <div class="chart-box" style="margin-top: 20px;">
            <div class="chart-title">Live Tuning</div>
            <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 15px; padding: 10px 0;">
                <div>
                    <label style="color: #888; font-size: 0.8em;">Smoothing (0.05-0.5)</label><br>
                    <input type="range" id="alphaSlider" min="5" max="50" value="10" style="width: 100%;">
                    <span id="alphaVal" style="color: #00d9ff;">0.10</span>
                </div>
                <div>
                    <label style="color: #888; font-size: 0.8em;">Min Threshold (0.5-20)</label><br>
                    <input type="range" id="threshSlider" min="5" max="200" value="30" style="width: 100%;">
                    <span id="threshVal" style="color: #00d9ff;">3.0</span>
                </div>
                <div>
                    <label style="color: #888; font-size: 0.8em;">Hysteresis ON (1-15)</label><br>
                    <input type="range" id="hystOnSlider" min="1" max="15" value="5" style="width: 100%;">
                    <span id="hystOnVal" style="color: #00d9ff;">5</span>
                </div>
                <div>
                    <label style="color: #888; font-size: 0.8em;">Hysteresis OFF (1-30)</label><br>
                    <input type="range" id="hystOffSlider" min="1" max="30" value="12" style="width: 100%;">
                    <span id="hystOffVal" style="color: #00d9ff;">12</span>
                </div>
            </div>
            <button onclick="recalibrate()" style="margin-top: 10px; padding: 8px 20px; background: #fbbf24; border: none; border-radius: 5px; color: black; cursor: pointer; font-weight: bold;">Recalibrate</button>
        </div>
        
        <div class="connection offline" id="connection">
            <span class="dot offline"></span>
            <span id="connText">Waiting for ESP32...</span>
            <button onclick="resetDashboard()" style="margin-left: 20px; padding: 8px 16px; background: #555; border: none; border-radius: 5px; color: white; cursor: pointer;">Reset Dashboard</button>
            <button onclick="restartSystem()" id="restartBtn" style="margin-left: 10px; padding: 8px 16px; background: #ff6600; border: none; border-radius: 5px; color: white; cursor: pointer; font-weight: bold;">Restart System</button>
        </div>
    </div>
    
    <script>
        const canvas = document.getElementById('chart');
        const ctx = canvas.getContext('2d');
        let motionHistory = [];
        
        function resizeCanvas() {
            const rect = canvas.parentElement.getBoundingClientRect();
            canvas.width = rect.width - 50;
            canvas.height = 120;
        }
        
        function drawChart() {
            const w = canvas.width;
            const h = canvas.height;
            
            ctx.fillStyle = 'rgba(0,0,0,0.3)';
            ctx.fillRect(0, 0, w, h);
            
            if (motionHistory.length < 2) return;
            
            const barWidth = w / 100;
            
            for (let i = 0; i < motionHistory.length; i++) {
                const x = i * barWidth;
                const isMotion = motionHistory[i] === 1;
                
                ctx.fillStyle = isMotion ? '#00ff64' : '#222';
                ctx.fillRect(x, 0, barWidth - 1, h);
            }
        }
        
        async function update() {
            try {
                const res = await fetch('/api/status');
                const data = await res.json();
                
                // Update motion display
                const box = document.getElementById('motionBox');
                const status = document.getElementById('motionStatus');
                
                if (data.motion) {
                    box.className = 'motion-box active';
                    status.className = 'motion-status detected';
                    status.textContent = 'MOTION!';
                } else {
                    box.className = 'motion-box';
                    status.className = 'motion-status idle';
                    status.textContent = 'NO MOTION';
                }
                
                // Update stats
                document.getElementById('rssi').textContent = data.rssi || '--';
                document.getElementById('pps').textContent = data.pps || '--';
                document.getElementById('total').textContent = data.packets ? data.packets.toLocaleString() : '--';
                
                // Update debug stats
                document.getElementById('variance').textContent = data.variance ? data.variance.toFixed(4) : '--';
                document.getElementById('threshold').textContent = data.threshold ? data.threshold.toFixed(4) : '--';
                const calEl = document.getElementById('calStatus');
                if (data.calibrated) {
                    calEl.textContent = 'READY';
                    calEl.style.color = '#00ff64';
                } else {
                    calEl.textContent = 'CALIBRATING...';
                    calEl.style.color = '#fbbf24';
                }
                
                // Update connection status
                const conn = document.getElementById('connection');
                const connText = document.getElementById('connText');
                const dot = conn.querySelector('.dot');
                
                if (data.connected) {
                    conn.className = 'connection online';
                    dot.className = 'dot online';
                    if (data.calibrated) {
                        connText.textContent = 'Connected - Ready for motion detection';
                    } else {
                        connText.textContent = 'Connected - CALIBRATING (keep area still!)';
                        dot.style.background = '#fbbf24';  // Yellow during calibration
                    }
                } else {
                    conn.className = 'connection offline';
                    dot.className = 'dot offline';
                    connText.textContent = 'Waiting for ESP32... (Is WiFi connected to ESP32-AP?)';
                }
                
                // Update chart
                motionHistory = data.motion_history || [];
                drawChart();
                
            } catch (e) {
                console.error('Update failed:', e);
            }
        }
        
        window.addEventListener('resize', resizeCanvas);
        resizeCanvas();
        
        async function resetDashboard() {
            try {
                await fetch('/api/reset');
                motionHistory = [];
                drawChart();
                console.log('Dashboard reset');
            } catch (e) {
                console.error('Reset failed:', e);
            }
        }
        
        async function restartSystem() {
            const btn = document.getElementById('restartBtn');
            const connText = document.getElementById('connText');
            
            btn.disabled = true;
            btn.textContent = 'Resetting AP...';
            btn.style.background = '#666';
            connText.textContent = 'Restarting devices...';
            
            try {
                const res = await fetch('/api/restart');
                const data = await res.json();
                console.log('Restart result:', data);
                
                // Clear dashboard data
                motionHistory = [];
                drawChart();
                
                // Show WiFi reconnection warning
                btn.textContent = 'Reconnect WiFi!';
                btn.style.background = '#ff9900';
                connText.textContent = 'WiFi disconnected! Reconnect to ESP32-AP, then wait for calibration...';
                
                // Start a countdown to remind user
                let countdown = 15;
                const countdownInterval = setInterval(() => {
                    countdown--;
                    if (countdown > 0) {
                        connText.textContent = `Reconnect to ESP32-AP WiFi... (${countdown}s)`;
                    } else {
                        clearInterval(countdownInterval);
                        btn.disabled = false;
                        btn.textContent = 'Restart System';
                        btn.style.background = '#ff6600';
                    }
                }, 1000);
                
            } catch (e) {
                console.error('Restart failed:', e);
                btn.textContent = 'Failed!';
                btn.style.background = '#ff0000';
                
                setTimeout(() => {
                    btn.disabled = false;
                    btn.textContent = 'Restart System';
                    btn.style.background = '#ff6600';
                }, 2000);
            }
        }
        
        // Tuning slider handlers
        function setupSlider(sliderId, valueId, apiParam, divisor = 1) {
            const slider = document.getElementById(sliderId);
            const valueSpan = document.getElementById(valueId);
            
            slider.addEventListener('input', () => {
                const val = slider.value / divisor;
                valueSpan.textContent = val.toFixed(divisor > 1 ? 2 : 0);
            });
            
            slider.addEventListener('change', () => {
                const val = slider.value / divisor;
                fetch(`/api/tune?${apiParam}=${val}`);
            });
        }
        
        setupSlider('alphaSlider', 'alphaVal', 'alpha', 100);
        setupSlider('threshSlider', 'threshVal', 'threshold', 10);
        setupSlider('hystOnSlider', 'hystOnVal', 'hyst_on', 1);
        setupSlider('hystOffSlider', 'hystOffVal', 'hyst_off', 1);
        
        async function recalibrate() {
            await fetch('/api/tune?recalibrate=1');
            console.log('Recalibration requested');
        }
        
        // Update every 100ms for responsive display
        setInterval(update, 100);
        update();
    </script>
</body>
</html>
"""


class DashboardHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass
    
    def do_GET(self):
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-type', 'text/html')
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode())
            
        elif self.path == '/api/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            
            with status_lock:
                response = {
                    "motion": status["motion"],
                    "rssi": status["rssi"],
                    "pps": status["pps"],
                    "packets": status["packets"],
                    "connected": status["connected"],
                    "calibrated": status["calibrated"],
                    "threshold": status["threshold"],
                    "variance": status["variance"],
                    "motion_history": list(status["motion_history"]),
                }
            
            self.wfile.write(json.dumps(response).encode())
            
        elif self.path == '/api/reset':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            with status_lock:
                # Reset all values in place
                status["motion"] = False
                status["rssi"] = 0
                status["pps"] = 0
                status["packets"] = 0
                status["last_update"] = 0
                status["connected"] = False
                status["calibrated"] = False
                status["threshold"] = 0
                status["variance"] = 0
                status["motion_history"].clear()
                status["rssi_history"].clear()
                status["pps_history"].clear()
            print("[API] Dashboard reset - cleared all data")
            
            self.wfile.write(json.dumps({"status": "reset"}).encode())
            
        elif self.path == '/api/restart':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            print("[API] Restart requested - resetting all ESP32 devices...")
            
            # Reset all devices
            results = restart_all_devices()
            
            # Also clear dashboard state
            with status_lock:
                status["motion"] = False
                status["rssi"] = 0
                status["pps"] = 0
                status["packets"] = 0
                status["last_update"] = 0
                status["connected"] = False
                status["calibrated"] = False
                status["threshold"] = 0
                status["variance"] = 0
                status["motion_history"].clear()
                status["rssi_history"].clear()
                status["pps_history"].clear()
            
            self.wfile.write(json.dumps({"status": "restarted", "devices": results}).encode())
            
        elif self.path.startswith('/api/tune'):
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            
            # Parse query parameters
            from urllib.parse import urlparse, parse_qs
            query = parse_qs(urlparse(self.path).query)
            
            result = {"status": "ok", "commands": []}
            
            if 'alpha' in query:
                cmd = f"alpha={query['alpha'][0]}"
                send_command_to_esp32(cmd)
                result["commands"].append(cmd)
            if 'threshold' in query:
                cmd = f"threshold={query['threshold'][0]}"
                send_command_to_esp32(cmd)
                result["commands"].append(cmd)
            if 'hyst_on' in query:
                cmd = f"hyst_on={query['hyst_on'][0]}"
                send_command_to_esp32(cmd)
                result["commands"].append(cmd)
            if 'hyst_off' in query:
                cmd = f"hyst_off={query['hyst_off'][0]}"
                send_command_to_esp32(cmd)
                result["commands"].append(cmd)
            if 'recalibrate' in query:
                send_command_to_esp32("recalibrate")
                result["commands"].append("recalibrate")
            
            self.wfile.write(json.dumps(result).encode())
        else:
            self.send_response(404)
            self.end_headers()


def main():
    print("=" * 50)
    print("  ESP32 CSI Motion Detection Dashboard")
    print("=" * 50)
    print(f"  UDP Status Port: {UDP_PORT}")
    print(f"  Web Dashboard:   http://localhost:{WEB_PORT}")
    print("=" * 50)
    print()
    print("1. Connect your PC to 'ESP32-AP' WiFi (password: esp32-ap)")
    print("2. Open http://localhost:8080 in your browser")
    print("3. Wave your hand between the two ESP32 boards!")
    print()
    
    # Start UDP receiver
    udp_thread = threading.Thread(target=udp_receiver, daemon=True)
    udp_thread.start()
    
    # Start web server
    server = HTTPServer(('0.0.0.0', WEB_PORT), DashboardHandler)
    print(f"[WEB] Dashboard running at http://localhost:{WEB_PORT}")
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.shutdown()


if __name__ == "__main__":
    main()
