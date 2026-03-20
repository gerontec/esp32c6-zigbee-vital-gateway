#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# build.sh – ESP32-C6 Firmware Build-Script
#
# Varianten (PROFILE):
#   zigbee   Zigbee-Koordinator + MR60BHA2 Radar → UART-Bridge  (Standard)
#   rcp      Thread Radio Co-Processor für otbr-agent via USB
#   matter   Matter over Thread Vital-Signs Sensor
#
# Verwendung:
#   ./build.sh [zigbee|rcp|matter] [flash [PORT]]
#
# Beispiele:
#   ./build.sh                          # Zigbee bauen
#   ./build.sh rcp                      # RCP bauen
#   ./build.sh rcp flash                # RCP bauen + flashen (auto Port)
#   ./build.sh zigbee flash /dev/ttyUSB0
#   ./build.sh matter flash /dev/ttyACM0
# ─────────────────────────────────────────────────────────────────────────────
set -e

PROFILE=${1:-zigbee}
ACTION=${2:-build}
PORT=${3:-}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── IDF / Matter Umgebung laden ───────────────────────────────────────────────
if [[ -z "$IDF_PATH" ]]; then
    if [[ -f "$HOME/esp-idf/export.sh" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/esp-idf/export.sh" > /dev/null 2>&1
    else
        echo "FEHLER: IDF_PATH nicht gesetzt und ~/esp-idf/export.sh nicht gefunden." >&2
        exit 1
    fi
fi

if [[ "$PROFILE" == "matter" && -z "$ESP_MATTER_PATH" ]]; then
    if [[ -f "$HOME/esp-matter/export.sh" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/esp-matter/export.sh" > /dev/null 2>&1
    else
        echo "FEHLER: ESP_MATTER_PATH nicht gesetzt." >&2
        exit 1
    fi
fi

# ── Profil → Build-Verzeichnis ────────────────────────────────────────────────
case "$PROFILE" in
    zigbee)
        BUILD_DIR="$SCRIPT_DIR"
        BINARY="build/zigbee-vital-sensor.bin"
        FLASH_ADDR="0x0"
        PROFILE_LABEL="Zigbee Koordinator"
        ;;
    rcp)
        BUILD_DIR="$SCRIPT_DIR/rcp"
        BINARY="build/esp32c6_rcp.bin"
        PROFILE_LABEL="Thread RCP (Radio Co-Processor)"
        ;;
    matter)
        BUILD_DIR="$SCRIPT_DIR/matter"
        BINARY="build/esp32c6_matter_vital_gateway.bin"
        PROFILE_LABEL="Matter over Thread Vital Sensor"
        ;;
    *)
        echo "Unbekanntes Profil: $PROFILE"
        echo "Gültig: zigbee | rcp | matter"
        exit 1
        ;;
esac

echo "╔══════════════════════════════════════════════════════╗"
echo "║  ESP32-C6 Build  –  $PROFILE_LABEL"
echo "╚══════════════════════════════════════════════════════╝"
echo "  Profil    : $PROFILE"
echo "  Verzeichnis: $BUILD_DIR"
echo "  IDF        : $IDF_PATH"
[[ "$PROFILE" == "matter" ]] && echo "  Matter SDK : $ESP_MATTER_PATH"
echo ""

cd "$BUILD_DIR"

# sdkconfig löschen wenn sich das Profil geändert hat
PROFILE_STAMP="build/.last_profile"
if [[ -f "$PROFILE_STAMP" ]] && [[ "$(cat "$PROFILE_STAMP")" != "$PROFILE" ]]; then
    echo "Profil gewechselt – sdkconfig wird zurückgesetzt …"
    rm -f sdkconfig
fi

# ── Bauen ─────────────────────────────────────────────────────────────────────
idf.py build

mkdir -p build
echo "$PROFILE" > "$PROFILE_STAMP"

echo ""
echo "✓ Build fertig: $BUILD_DIR/$BINARY"

# ── Flashen (optional) ────────────────────────────────────────────────────────
if [[ "$ACTION" == "flash" ]]; then
    FLASH_CMD="idf.py"
    if [[ -n "$PORT" ]]; then
        FLASH_CMD="$FLASH_CMD -p $PORT"
    fi
    echo ""
    echo "Flash auf ${PORT:-auto} …"
    $FLASH_CMD flash
    echo "✓ Flash abgeschlossen"

    # Serielle Console öffnen (nur bei interaktivem TTY, nicht für rcp)
    if [[ "$PROFILE" != "rcp" ]] && [[ -t 0 ]]; then
        echo ""
        echo "Serielle Console (Ctrl+] zum Beenden):"
        if [[ -n "$PORT" ]]; then
            idf.py -p "$PORT" monitor
        else
            idf.py monitor
        fi
    fi
fi
