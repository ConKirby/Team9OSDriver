#!/bin/bash
# Load the echo_robot kernel module and create device node
#
# Usage:
#   sudo ./load_module.sh                    # sim mode (default)
#   sudo ./load_module.sh sim_mode=0         # real hardware
#   sudo ./load_module.sh sim_mode=0 gpio_up=17 gpio_down=27

set -e

MODULE_NAME="echo_robot"
DEVICE_NAME="echo_robot"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_PATH="${SCRIPT_DIR}/../module/${MODULE_NAME}.ko"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: Please run as root (sudo)"
    exit 1
fi

# Check if module file exists
if [ ! -f "$MODULE_PATH" ]; then
    echo "ERROR: Module not found at $MODULE_PATH"
    echo "Build it first:  cd module && make"
    exit 1
fi

# Unload if already loaded
if lsmod | grep -q "^${MODULE_NAME}"; then
    echo "Module already loaded — removing first..."
    rmmod "$MODULE_NAME"
    sleep 0.5
fi

# Load the module with any extra parameters
echo "Loading ${MODULE_NAME}..."
if [ $# -gt 0 ]; then
    echo "  Parameters: $*"
fi
insmod "$MODULE_PATH" "$@"

# Wait for udev to create the device node
sleep 1

# Fallback: create device node manually if udev didn't
if [ ! -c "/dev/${DEVICE_NAME}" ]; then
    echo "Device node not created by udev — creating manually..."
    MAJOR=$(grep "$DEVICE_NAME" /proc/devices | awk '{print $1}')
    if [ -z "$MAJOR" ]; then
        echo "ERROR: Failed to find major number in /proc/devices"
        rmmod "$MODULE_NAME"
        exit 1
    fi
    mknod "/dev/${DEVICE_NAME}" c "$MAJOR" 0
    chmod 666 "/dev/${DEVICE_NAME}"
fi

# Make device readable/writable by all (for non-root app testing)
chmod 666 "/dev/${DEVICE_NAME}"

echo ""
echo "=== Module loaded successfully ==="
echo "  Device:  /dev/${DEVICE_NAME}"
echo "  Stats:   /proc/echo_stats"
echo ""
echo "Quick checks:"
echo "  dmesg | tail -20"
echo "  cat /proc/echo_stats"
echo ""
