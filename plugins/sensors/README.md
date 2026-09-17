# Sensors Plugin

System monitoring plugin that exposes Linux system metrics as ESPHome sensor entities.

## Overview

This plugin reads system information from Linux `/proc` filesystem, `/sys/class/thermal`, and `/etc/fstab` to provide real-time monitoring sensors:

- **CPU Usage** - Total CPU usage percentage
- **Load Average** - 1, 5, and 15 minute load averages
- **Memory Usage** - RAM usage percentage, total, and available
- **Temperature** - CPU and GPU temperature (from thermal zones)
- **Network Interfaces** - RX/TX bytes per interface
- **Disk Usage** - Usage percentage for mounted filesystems from `/etc/fstab`

## Features

- **Real-time monitoring** - Background thread updates sensors periodically
- **Dynamic interfaces** - Network interfaces detected automatically
- **Selective disk monitoring** - Only shows disks defined in `/etc/fstab`
- **Excludes virtual interfaces** - Filters out vth*, lo, docker*, zth* interfaces
- **Excludes swap/cache** - Swap and cache filesystem types are excluded

## Sensor Entities

### CPU, Memory, and Temperature (10-second updates)

| Entity | Key | Unit | Description |
|--------|-----|------|-------------|
| cpu_usage | 1000 | % | CPU usage percentage |
| load_1min | 1001 | - | 1-minute load average |
| load_5min | 1002 | - | 5-minute load average |
| load_15min | 1003 | - | 15-minute load average |
| memory_usage | 1004 | % | Memory usage percentage |
| memory_total | 1005 | MB | Total memory |
| memory_available | 1006 | MB | Available memory |
| cpu_temperature | 1007 | °C | CPU temperature |
| gpu_temperature | 1008 | °C | GPU temperature |

### Network Interfaces (1-minute updates)

| Entity | Key Range | Unit | Description |
|--------|-----------|------|-------------|
| {iface}_rx_bytes | 1100+ | B | Received bytes |
| {iface}_tx_bytes | 1101+ | B | Transmitted bytes |

### Disk Usage (5-minute updates)

| Entity | Key Range | Unit | Description |
|--------|-----------|------|-------------|
| disk_{mountpoint} | 1200+ | % | Disk usage percentage |

## Excluded Interfaces

The following network interface prefixes are excluded:
- `vth` - Virtual ethernet interfaces
- `lo` - Loopback
- `docker` - Docker bridge interfaces
- `zth` - ZeroTier interfaces

## Excluded Filesystem Types

The following filesystem types are excluded from disk monitoring:
- Virtual: proc, sysfs, devtmpfs, tmpfs, devpts, cgroup, etc.
- Swap: swap
- Cache/ReadOnly: squashfs, iso9660, udf, vfat, exfat, ntfs, fuseblk

## Temperature Sensors

Temperature is read from:
- `/sys/class/thermal/thermal_zone0/temp` - CPU temperature
- `/sys/class/thermal/thermal_zone1/temp` - GPU temperature

Values are in millidegrees Celsius and converted to degrees.

## Requirements

- Linux with `/proc` filesystem
- Read access to `/proc/stat`, `/proc/loadavg`, `/proc/meminfo`, `/proc/net/dev`
- Read access to `/sys/class/thermal/thermal_zone*`
- Read access to `/etc/fstab` and `/proc/mounts`

## Building

The plugin has no external dependencies:

```bash
cd ~/github/esphome-linux
meson setup build
meson compile -C build
```

## Files

- `sensors_plugin.c` - Main plugin implementation
- `meson.build` - Build configuration
- `README.md` - This file

## Usage

The plugin starts automatically when Home Assistant connects and subscribes to states. No configuration is needed.

### Home Assistant Integration

Once connected, sensors appear in Home Assistant as:
- Sensor entities with appropriate device classes
- Historical data tracking for network counters (total_increasing state class)
- Real-time updates for CPU, memory, temperature, and load average

## Testing

```bash
# Verify /proc files are readable
cat /proc/stat | head -1
cat /proc/loadavg
cat /proc/meminfo | head -5
cat /proc/net/dev

# Check thermal zones
ls /sys/class/thermal/
cat /sys/class/thermal/thermal_zone0/temp

# Check fstab entries
cat /etc/fstab

# Check mounted filesystems
mount | grep "^/dev"

# Run esphome-linux with sensors plugin
./build/esphome-linux
```

## Troubleshooting

**No temperature sensors appearing:**
- Check if thermal zones exist: `ls /sys/class/thermal/`
- Verify read access: `cat /sys/class/thermal/thermal_zone0/temp`
- Some systems may have different zone names

**No disk sensors appearing:**
- Verify disks are defined in `/etc/fstab`
- Check that mounts are actual physical devices (not tmpfs, proc, etc.)
- Ensure read access to `/proc/mounts`

**Network interfaces not showing:**
- Check `/proc/net/dev` for available interfaces
- Verify interface names don't start with excluded prefixes

**CPU usage shows 0:**
- First reading establishes baseline - subsequent readings show actual usage
- Wait for at least one update interval (10 seconds)

## Future Enhancements

- Disk I/O rates (read/write bytes per second)
- Process count
- Uptime sensor
- Additional thermal zones
- Custom update intervals via configuration

## License

MIT - Same as main project
