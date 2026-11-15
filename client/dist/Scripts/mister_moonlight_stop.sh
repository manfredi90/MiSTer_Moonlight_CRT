#!/bin/bash
# Stops MiSTer Moonlight if it is still running
killall mister_moonlight 2>/dev/null && echo "MiSTer Moonlight stopped" || echo "MiSTer Moonlight is not running"
