#!/bin/bash
# Unload the echo_robot kernel module

set -e

MODULE_NAME="echo_robot"
DEVICE_NAME="echo_robot"

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (sudo)"
    exit 1
fi

if ! lsmod | grep -q "^${MODULE_NAME}"; then
    echo "Module ${MODULE_NAME} is not loaded"
    exit 0
fi

echo "Unloading ${MODULE_NAME}..."
rmmod "$MODULE_NAME"

# Remove device node if it still exists
if [ -c "/dev/${DEVICE_NAME}" ]; then
    rm -f "/dev/${DEVICE_NAME}"
fi

echo "Module unloaded successfully!"
echo ""
echo "Check dmesg for cleanup messages:"
echo "  dmesg | tail -10"
