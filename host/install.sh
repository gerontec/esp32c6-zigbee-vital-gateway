#!/usr/bin/env bash
# install.sh – ESP32-C6 Gateway Service nativ installieren (kein Docker nötig)
# Aufruf: bash install.sh   (als normaler User, kein sudo erforderlich)
#
# Voraussetzungen auf dem Ziel-Host:
#   - Python 3.9+            (apt: python3 python3-venv)
#   - Mosquitto läuft        (apt: mosquitto)    ← oder eigener Broker
#   - systemd (User-Session) für automatischen Start
#
# Docker-Alternative: siehe docker-compose.yml
set -euo pipefail

INSTALL_DIR="$HOME/esp32-gw"
VENV="$INSTALL_DIR/venv"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_NAME="esp32gw"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "══════════════════════════════════════════════════════"
echo " ESP32-C6 Gateway Service – Native Installation"
echo "══════════════════════════════════════════════════════"

# ── 1. Dateien an Zielort kopieren ───────────────────────────────────────────
echo "[1/4] Dateien installieren nach $INSTALL_DIR …"
mkdir -p "$INSTALL_DIR"
# Nur kopieren wenn Quelle ≠ Ziel (verhindert "same file"-Fehler bei in-place Install)
if [ "$(realpath "$SCRIPT_DIR")" != "$(realpath "$INSTALL_DIR")" ]; then
    cp "$SCRIPT_DIR/gateway_service.py" "$INSTALL_DIR/"
    cp "$SCRIPT_DIR/requirements.txt"   "$INSTALL_DIR/"
else
    echo "  → In-place Installation, kein Kopieren nötig"
fi

# ── 2. Python Virtual Environment ───────────────────────────────────────────
echo "[2/4] Python-Umgebung einrichten …"
python3 -m venv "$VENV"
"$VENV/bin/pip" install --quiet --upgrade pip
"$VENV/bin/pip" install --quiet -r "$INSTALL_DIR/requirements.txt"
PY_VER=$("$VENV/bin/python" --version 2>&1)
echo "  → $PY_VER + paho-mqtt installiert"

# ── 3. systemd User-Service einrichten ──────────────────────────────────────
echo "[3/4] systemd User-Service einrichten …"
mkdir -p "$SERVICE_DIR"

# Umgebungsvariablen aus Konfigurationsdatei (optional)
CONFIG_FILE="$INSTALL_DIR/gateway.env"
if [ ! -f "$CONFIG_FILE" ]; then
    cat > "$CONFIG_FILE" <<EOF
MQTT_HOST=127.0.0.1
MQTT_PORT=1883
WEB_PORT=8080
DEVICES_FILE=$INSTALL_DIR/devices.json
LOG_LEVEL=INFO
EOF
    echo "  → Konfiguration angelegt: $CONFIG_FILE"
fi

cat > "$SERVICE_DIR/${SERVICE_NAME}.service" <<EOF
[Unit]
Description=ESP32-C6 Radio Bridge Gateway Service
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
ExecStart=$VENV/bin/python $INSTALL_DIR/gateway_service.py
EnvironmentFile=$CONFIG_FILE
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=$SERVICE_NAME

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable "$SERVICE_NAME"
systemctl --user restart "$SERVICE_NAME"

# Lingering aktivieren (Service startet auch ohne Login-Session)
loginctl enable-linger "$(whoami)" 2>/dev/null || true

echo "  → Service aktiv"

# ── 4. Status prüfen ─────────────────────────────────────────────────────────
echo "[4/4] Status prüfen …"
sleep 2
if systemctl --user is-active --quiet "$SERVICE_NAME"; then
    echo "  ✓ $SERVICE_NAME läuft"
else
    echo "  ✗ Fehler – Log:"
    journalctl --user -u "$SERVICE_NAME" --no-pager -n 20
    exit 1
fi

# ── Ausgabe ──────────────────────────────────────────────────────────────────
HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo ""
echo "  Web-Dashboard : http://${HOST_IP:-localhost}:8080/"
echo "  JSON-API      : http://${HOST_IP:-localhost}:8080/api/state"
echo "  Konfiguration : $CONFIG_FILE"
echo "  Logs          : journalctl --user -fu $SERVICE_NAME"
echo ""
echo "  Permit Join öffnen:"
echo "    mosquitto_pub -t gw/<mac>/cmd/permit_join -m 180"
echo ""
echo "  Service-Befehle:"
echo "    systemctl --user status $SERVICE_NAME"
echo "    systemctl --user restart $SERVICE_NAME"
echo "    systemctl --user stop $SERVICE_NAME"
echo ""
echo "══════════════════════════════════════════════════════"
