#!/bin/bash
# Load the echo_robot kernel module and create device node

set -e

MODULE_NAME="echo_robot"
DEVICE_NAME="echo_robot"
MODULE_PATH="../module/${MODULE_NAME}.ko"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo)"
    exit 1
fi

# Check if module file exists
if [ ! -f "$MODULE_PATH" ]; then
    echo "Module not found at $MODULE_PATH"
    echo "Please build the module first: cd ../module && make"
    exit 1
fi

# Unload if already loaded
if lsmod | grep -q "^${MODULE_NAME}"; then
    echo "Module already loaded, removing first..."
    rmmod "$MODULE_NAME"
fi

# Load the module
# Default: simulation mode. Pass sim_mode=0 for real hardware.
echo "Loading ${MODULE_NAME}..."
insmod "$MODULE_PATH" "$@"

# Wait for udev to create the device
sleep 1

# Check if device was created by udev
if [ ! -c "/dev/${DEVICE_NAME}" ]; then
    echo "Device node not created by udev, creating manually..."
    MAJOR=$(grep "$DEVICE_NAME" /proc/devices | awk '{print $1}')
    if [ -z "$MAJOR" ]; then
        echo "Failed to find major number"
        rmmod "$MODULE_NAME"
        exit 1
    fi
    mknod "/dev/${DEVICE_NAME}" c "$MAJOR" 0
    chmod 666 "/dev/${DEVICE_NAME}"
fi

echo "Module loaded successfully!"
echo "Device: /dev/${DEVICE_NAME}"
echo ""
echo "Check status:"
echo "  cat /proc/echo_stats"
echo "  dmesg | tail -20"
