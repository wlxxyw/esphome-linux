/**
 * @file sensors_plugin.c
 * @brief Sensors Plugin for ESPHome
 *
 * Provides system monitoring sensors:
 * - CPU usage percentage
 * - Load average (1, 5, 15 minutes)
 * - Memory usage percentage
 * - Network interface usage (RX/TX bytes)
 * - Disk usage percentage (from /etc/fstab)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include "../../src/include/esphome_plugin.h"
#include "../../src/include/esphome_api.h"
#include "../../src/include/esphome_proto.h"
#include "../../src/include/sensor_proto.h"

/* Sensor key ranges */
#define SENSOR_KEY_CPU_USAGE         1000
#define SENSOR_KEY_LOAD_1MIN         1001
#define SENSOR_KEY_LOAD_5MIN         1002
#define SENSOR_KEY_LOAD_15MIN        1003
#define SENSOR_KEY_MEMORY_USAGE      1004
#define SENSOR_KEY_MEMORY_TOTAL      1005
#define SENSOR_KEY_MEMORY_AVAILABLE  1006
#define SENSOR_KEY_TEMP_BASE         1010  /* + thermal zone index */
#define SENSOR_KEY_NETWORK_BASE      1100  /* + interface index */
#define SENSOR_KEY_DISK_BASE         1200  /* + disk index */

/* Maximum sensors */
#define MAX_NETWORK_INTERFACES      32
#define MAX_DISK_MOUNTS             32
#define MAX_THERMAL_ZONES           16
#define MAX_MOUNT_POINT_PATH        255

/* Update intervals (in milliseconds) */
#define CPU_MEMORY_UPDATE_INTERVAL_MS   10000   /* 10 seconds */
#define NETWORK_UPDATE_INTERVAL_MS      60000   /* 1 minute */
#define DISK_UPDATE_INTERVAL_MS         300000  /* 5 minutes */

/* Network interface exclusion patterns */
static const char *exclude_prefixes[] = {
    "vth", "veth", "lo", "docker", "zth", NULL
};

/* Network interface state */
typedef struct {
    char name[64];
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t key_rx;
    uint32_t key_tx;
} network_iface_t;

/* Disk mount state */
typedef struct {
    char mountpoint[256];
    char filesystem[256];
    uint32_t key;
} disk_mount_t;

/* Thermal zone state */
typedef struct {
    char name[64];          /* e.g. "thermal_zone0" */
    char type[128];         /* e.g. "cpu-thermal" from /sys/class/thermal/thermal_zoneX/type */
    uint32_t key;
} thermal_zone_t;

/* Plugin state */
typedef struct {
    /* CPU tracking */
    uint64_t prev_idle;
    uint64_t prev_total;

    /* Network interfaces */
    network_iface_t interfaces[MAX_NETWORK_INTERFACES];
    int interface_count;

    /* Disk mounts */
    disk_mount_t disks[MAX_DISK_MOUNTS];
    int disk_count;

    /* Thermal zones */
    thermal_zone_t thermal_zones[MAX_THERMAL_ZONES];
    int thermal_zone_count;

    /* Threading */
    pthread_t update_thread;
    volatile bool update_thread_running;
    int shutdown_pipe[2];  /* Pipe to wake up thread from sleep */
    esphome_plugin_context_t *ctx;

    /* Entity keys registered */
    bool entities_registered;
} sensors_state_t;

/**
 * Check if interface name should be excluded
 */
static bool should_exclude_interface(const char *name) {
    for (int i = 0; exclude_prefixes[i] != NULL; i++) {
        if (strncmp(name, exclude_prefixes[i], strlen(exclude_prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Read CPU usage from /proc/stat
 * Returns percentage (0-100)
 */
static float read_cpu_usage(sensors_state_t *state) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0f;

    char line[256];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return 0.0f;
    }
    fclose(f);

    /* Parse "cpu  user nice system idle ..." */
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return 0.0f;
    }

    uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
    uint64_t idle_time = idle + iowait;

    uint64_t diff_total = total - state->prev_total;
    uint64_t diff_idle = idle_time - state->prev_idle;

    state->prev_total = total;
    state->prev_idle = idle_time;

    if (diff_total == 0) return 0.0f;

    return 100.0f * (1.0f - (float)diff_idle / (float)diff_total);
}

/**
 * Read load average from /proc/loadavg
 */
static void read_load_average(float *load1, float *load5, float *load15) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) {
        *load1 = *load5 = *load15 = 0.0f;
        return;
    }

    fscanf(f, "%f %f %f", load1, load5, load15);
    fclose(f);
}

/**
 * Read memory usage from /proc/meminfo
 * Returns percentage (0-100)
 */
static float read_memory_usage(uint64_t *total_kb, uint64_t *available_kb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        *total_kb = *available_kb = 0;
        return 0.0f;
    }

    char line[256];
    uint64_t mem_total = 0, mem_available = 0, mem_free = 0, buffers = 0, cached = 0;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) continue;
        if (sscanf(line, "Buffers: %lu kB", &buffers) == 1) continue;
        if (sscanf(line, "Cached: %lu kB", &cached) == 1) continue;
    }
    fclose(f);

    *total_kb = mem_total;

    /* Use MemAvailable if present (kernel 3.14+), otherwise estimate */
    if (mem_available > 0) {
        *available_kb = mem_available;
    } else {
        *available_kb = mem_free + buffers + cached;
    }

    if (mem_total == 0) return 0.0f;

    return 100.0f * (1.0f - (float)*available_kb / (float)mem_total);
}

/**
 * Read temperature from thermal zone
 * Returns temperature in Celsius (value from sysfs is in millidegrees)
 */
static float read_temperature(const char *thermal_zone) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", thermal_zone);

    FILE *f = fopen(path, "r");
    if (!f) return 0.0f;

    int temp_millidegrees;
    if (fscanf(f, "%d", &temp_millidegrees) != 1) {
        fclose(f);
        return 0.0f;
    }
    fclose(f);

    return (float)temp_millidegrees / 1000.0f;
}

/**
 * Read thermal zone type from sysfs
 */
static void read_thermal_zone_type(const char *zone_name, char *type_buf, size_t buf_size) {
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", zone_name);

    FILE *f = fopen(path, "r");
    if (!f) {
        strncpy(type_buf, zone_name, buf_size - 1);
        type_buf[buf_size - 1] = '\0';
        return;
    }

    if (fgets(type_buf, buf_size, f) == NULL) {
        strncpy(type_buf, zone_name, buf_size - 1);
        type_buf[buf_size - 1] = '\0';
    } else {
        /* Trim trailing newline */
        size_t len = strlen(type_buf);
        while (len > 0 && (type_buf[len-1] == '\n' || type_buf[len-1] == '\r')) {
            type_buf[--len] = '\0';
        }
    }
    fclose(f);
}

/**
 * Scan and discover all thermal zones from /sys/class/thermal/
 */
static void scan_thermal_zones(sensors_state_t *state) {
    int count = 0;

    for (int i = 0; i < MAX_THERMAL_ZONES && count < MAX_THERMAL_ZONES; i++) {
        char zone_name[64];
        snprintf(zone_name, sizeof(zone_name), "thermal_zone%d", i);

        /* Check if temp file exists */
        char temp_path[256];
        snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp", zone_name);

        FILE *f = fopen(temp_path, "r");
        if (!f) continue;
        fclose(f);

        /* Read zone type */
        char type[128];
        read_thermal_zone_type(zone_name, type, sizeof(type));

        printf("[sensors] Found thermal zone: %s (type=%s)\n", zone_name, type);

        strncpy(state->thermal_zones[count].name, zone_name,
                sizeof(state->thermal_zones[count].name) - 1);
        strncpy(state->thermal_zones[count].type, type,
                sizeof(state->thermal_zones[count].type) - 1);
        state->thermal_zones[count].key = SENSOR_KEY_TEMP_BASE + count;
        count++;
    }

    state->thermal_zone_count = count;
    printf("[sensors] Found %d thermal zone(s)\n", count);
}

/**
 * Read network interfaces from /proc/net/dev
 */
static void read_network_stats(sensors_state_t *state) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;

    char line[256];

    /* Skip first two header lines */
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);

    int idx = 0;
    while (fgets(line, sizeof(line), f) && idx < MAX_NETWORK_INTERFACES) {
        char raw_name[64];
        char name[64];
        unsigned long long rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo, rx_frame, rx_compressed, rx_multicast;
        unsigned long long tx_bytes, tx_packets, tx_errs, tx_drop, tx_fifo, tx_colls, tx_carrier, tx_compressed;

        /* Parse "name: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame rx_compressed rx_multicast
         *        tx_bytes tx_packets tx_errs tx_drop tx_fifo tx_colls tx_carrier tx_compressed" */
        int parsed = sscanf(line, " %63[^:]: %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   raw_name,
                   &rx_bytes, &rx_packets, &rx_errs, &rx_drop, &rx_fifo, &rx_frame, &rx_compressed, &rx_multicast,
                   &tx_bytes, &tx_packets, &tx_errs, &tx_drop, &tx_fifo, &tx_colls, &tx_carrier, &tx_compressed);

        if (parsed < 10) {
            continue;
        }

        /* Trim leading/trailing spaces from name */
        strncpy(name, raw_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        char *p = name;
        while (*p == ' ' || *p == '\t') p++;
        if (p != name) {
            memmove(name, p, strlen(p) + 1);
        }
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == ' ' || name[len-1] == '\t' || name[len-1] == '\n')) {
            name[--len] = '\0';
        }

        printf("[sensors] parsed='%s' rx=%llu tx=%llu\n", name, rx_bytes, tx_bytes);

        /* Skip excluded interfaces */
        if (should_exclude_interface(name)) {
            printf("[sensors] excluding: %s\n", name);
            continue;
        }

        /* Check if interface already exists */
        int found = -1;
        for (int i = 0; i < state->interface_count; i++) {
            if (strcmp(state->interfaces[i].name, name) == 0) {
                found = i;
                break;
            }
        }

        if (found >= 0) {
            /* Update existing interface */
            state->interfaces[found].rx_bytes = rx_bytes;
            state->interfaces[found].tx_bytes = tx_bytes;
            idx++;
        } else if (idx < MAX_NETWORK_INTERFACES) {
            /* Add new interface */
            strncpy(state->interfaces[idx].name, name, sizeof(state->interfaces[idx].name) - 1);
            state->interfaces[idx].rx_bytes = rx_bytes;
            state->interfaces[idx].tx_bytes = tx_bytes;
            state->interfaces[idx].key_rx = SENSOR_KEY_NETWORK_BASE + idx * 2;
            state->interfaces[idx].key_tx = SENSOR_KEY_NETWORK_BASE + idx * 2 + 1;
            idx++;
        }
    }

    state->interface_count = idx;
    fclose(f);
}

/**
 * Check if mountpoint is in /etc/fstab and is a real filesystem
 */
static bool is_fstab_mount(const char *mountpoint, const char *fstype) {
    char mountpoint_copy[MAX_MOUNT_POINT_PATH];
    char fstype_copy[64];

    snprintf(mountpoint_copy,
             sizeof(mountpoint_copy),
             "%s",
             mountpoint);

    snprintf(fstype_copy,
             sizeof(fstype_copy),
             "%s",
             fstype);

    /* Skip virtual filesystems */
    if (strcmp(fstype_copy, "proc") == 0 ||
        strcmp(fstype_copy, "sysfs") == 0 ||
        strcmp(fstype_copy, "devtmpfs") == 0 ||
        strcmp(fstype_copy, "tmpfs") == 0 ||
        strcmp(fstype_copy, "devpts") == 0 ||
        strcmp(fstype_copy, "cgroup") == 0 ||
        strcmp(fstype_copy, "cgroup2") == 0 ||
        strcmp(fstype_copy, "pstore") == 0 ||
        strcmp(fstype_copy, "securityfs") == 0 ||
        strcmp(fstype_copy, "debugfs") == 0 ||
        strcmp(fstype_copy, "tracefs") == 0 ||
        strcmp(fstype_copy, "hugetlbfs") == 0 ||
        strcmp(fstype_copy, "mqueue") == 0 ||
        strcmp(fstype_copy, "configfs") == 0 ||
        strcmp(fstype_copy, "binfmt_misc") == 0 ||
        strcmp(fstype_copy, "autofs") == 0 ||
        strcmp(fstype_copy, "rpc_pipefs") == 0 ||
        strcmp(fstype_copy, "nfsd") == 0 ||
        strcmp(fstype_copy, "efivarfs") == 0 ||
        strncmp(fstype_copy, "fuse.", 5) == 0 ||
        strncmp(fstype_copy, "overlay", 7) == 0) {
            printf("[sensors] skip virtual filesystems mount point %s\n", mountpoint_copy);
            return false;
    }

    /* Skip swap and cache filesystems */
    if (strcmp(fstype_copy, "swap") == 0 ||
        strcmp(fstype_copy, "squashfs") == 0 ||
        strcmp(fstype_copy, "iso9660") == 0 ||
        strcmp(fstype_copy, "udf") == 0 ||
        strcmp(fstype_copy, "vfat") == 0 ||
        strcmp(fstype_copy, "exfat") == 0 ||
        strcmp(fstype_copy, "ntfs") == 0 ||
        strcmp(fstype_copy, "fuseblk") == 0 ||
        strcmp(fstype_copy, "fuse.snapfuse") == 0) {
            printf("[sensors] skip cache filesystems mount point %s\n", mountpoint_copy);
            return false;
    }

    /* Check if it's in fstab */
    FILE *fstab = setmntent("/etc/fstab", "r");
    if (!fstab) {
        printf("[sensors] OPEN /etc/fstab FAILED!!!");
        return false;
    }

    struct mntent *mnt;
    bool found = false;

    while ((mnt = getmntent(fstab)) != NULL) {
        if (strcmp(mnt->mnt_dir, mountpoint_copy) == 0) {
            if (strcmp(mnt->mnt_type, fstype_copy) == 0) {
                printf("[sensors] find mountpoint %s(%s)\n", mountpoint_copy, fstype_copy);
                found = true;
                break;
            }
        }
    }

    endmntent(fstab);
    return found;
}

/**
 * Read disk mounts from /proc/mounts
 */
static void read_disk_mounts(sensors_state_t *state) {
    FILE *f = setmntent("/proc/mounts", "r");
    if (!f) return;

    struct mntent *mnt;
    int idx = 0;

    while ((mnt = getmntent(f)) != NULL && idx < MAX_DISK_MOUNTS) {
        /* Skip non-physical devices */
        if (strncmp(mnt->mnt_fsname, "/dev/", 5) != 0) {
            continue;
        }

        /* Check if it's in fstab */
        if (!is_fstab_mount(mnt->mnt_dir, mnt->mnt_type)) {
            continue;
        }

        /* Check if already tracked */
        int found = -1;
        for (int i = 0; i < state->disk_count; i++) {
            if (strcmp(state->disks[i].mountpoint, mnt->mnt_dir) == 0) {
                found = i;
                break;
            }
        }

        if (found < 0) {
            strncpy(state->disks[idx].mountpoint, mnt->mnt_dir,
                    sizeof(state->disks[idx].mountpoint) - 1);
            strncpy(state->disks[idx].filesystem, mnt->mnt_fsname,
                    sizeof(state->disks[idx].filesystem) - 1);
            state->disks[idx].key = SENSOR_KEY_DISK_BASE + idx;
            idx++;
        }
    }

    state->disk_count = idx;
    endmntent(f);
}

/**
 * Read disk usage for a mountpoint
 * Returns percentage (0-100)
 */
static float read_disk_usage(const char *mountpoint) {
    struct statvfs stat;
    if (statvfs(mountpoint, &stat) != 0) {
        return 0.0f;
    }

    unsigned long long total = stat.f_blocks * stat.f_frsize;
    unsigned long long free_space = stat.f_bavail * stat.f_frsize;
    unsigned long long used = total - free_space;

    if (total == 0) return 0.0f;

    return 100.0f * (float)used / (float)total;
}

/**
 * Encode and send a sensor state update
 */
static void send_sensor_state(esphome_plugin_context_t *ctx, uint32_t key, float value) {
    sensor_state_response_t state_msg = {
        .key = key,
        .state = value,
        .missing_state = false
    };

    uint8_t buf[64];
    size_t len = sensor_encode_state_response(buf, sizeof(buf), &state_msg);

    if (len > 0) {
        esphome_plugin_send_message(ctx, ESPHOME_MSG_SENSOR_STATE_RESPONSE, buf, len);
    }
}

/**
 * Update all sensors
 */
static void update_sensors(sensors_state_t *state) {
    esphome_plugin_context_t *ctx = state->ctx;

    /* CPU usage */
    float cpu_usage = read_cpu_usage(state);
    send_sensor_state(ctx, SENSOR_KEY_CPU_USAGE, cpu_usage);

    /* Load average */
    float load1, load5, load15;
    read_load_average(&load1, &load5, &load15);
    send_sensor_state(ctx, SENSOR_KEY_LOAD_1MIN, load1);
    send_sensor_state(ctx, SENSOR_KEY_LOAD_5MIN, load5);
    send_sensor_state(ctx, SENSOR_KEY_LOAD_15MIN, load15);

    /* Memory usage */
    uint64_t mem_total, mem_available;
    float mem_usage = read_memory_usage(&mem_total, &mem_available);
    send_sensor_state(ctx, SENSOR_KEY_MEMORY_USAGE, mem_usage);
    send_sensor_state(ctx, SENSOR_KEY_MEMORY_TOTAL, (float)(mem_total / 1024));  /* Convert to MB */
    send_sensor_state(ctx, SENSOR_KEY_MEMORY_AVAILABLE, (float)(mem_available / 1024));  /* Convert to MB */

    /* Temperature sensors */
    for (int i = 0; i < state->thermal_zone_count; i++) {
        float temp = read_temperature(state->thermal_zones[i].name);
        send_sensor_state(ctx, state->thermal_zones[i].key, temp);
    }

    /* Network interfaces */
    read_network_stats(state);
    printf("[sensors] network interfaces count: %d\n", state->interface_count);
    for (int i = 0; i < state->interface_count; i++) {
        printf("[sensors] sending %s: rx=%llu tx=%llu key_rx=%u key_tx=%u\n",
               state->interfaces[i].name,
               state->interfaces[i].rx_bytes,
               state->interfaces[i].tx_bytes,
               state->interfaces[i].key_rx,
               state->interfaces[i].key_tx);
        send_sensor_state(ctx, state->interfaces[i].key_rx,
                         (float)state->interfaces[i].rx_bytes);
        send_sensor_state(ctx, state->interfaces[i].key_tx,
                         (float)state->interfaces[i].tx_bytes);
    }

    /* Disk usage */
    read_disk_mounts(state);
    for (int i = 0; i < state->disk_count; i++) {
        float disk_usage = read_disk_usage(state->disks[i].mountpoint);
        send_sensor_state(ctx, state->disks[i].key, disk_usage);
    }
}

/**
 * Background update thread
 */
static void *update_thread_func(void *arg) {
    sensors_state_t *state = (sensors_state_t *)arg;
    struct timespec last_cpu_mem = {0, 0};
    struct timespec last_network = {0, 0};
    struct timespec last_disk = {0, 0};

    /* Initialize timestamps */
    clock_gettime(CLOCK_MONOTONIC, &last_cpu_mem);
    last_network = last_cpu_mem;
    last_disk = last_cpu_mem;

    /* Read initial CPU stats to establish baseline */
    read_cpu_usage(state);

    while (state->update_thread_running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* CPU and memory update (10 seconds) */
        uint64_t elapsed_cpu = (now.tv_sec - last_cpu_mem.tv_sec) * 1000 +
                               (now.tv_nsec - last_cpu_mem.tv_nsec) / 1000000;
        if (elapsed_cpu >= CPU_MEMORY_UPDATE_INTERVAL_MS) {
            float cpu_usage = read_cpu_usage(state);
            send_sensor_state(state->ctx, SENSOR_KEY_CPU_USAGE, cpu_usage);

            uint64_t mem_total, mem_available;
            float mem_usage = read_memory_usage(&mem_total, &mem_available);
            send_sensor_state(state->ctx, SENSOR_KEY_MEMORY_USAGE, mem_usage);
            send_sensor_state(state->ctx, SENSOR_KEY_MEMORY_TOTAL, (float)(mem_total / 1024));
            send_sensor_state(state->ctx, SENSOR_KEY_MEMORY_AVAILABLE, (float)(mem_available / 1024));

            /* Also update load average with CPU interval */
            float load1, load5, load15;
            read_load_average(&load1, &load5, &load15);
            send_sensor_state(state->ctx, SENSOR_KEY_LOAD_1MIN, load1);
            send_sensor_state(state->ctx, SENSOR_KEY_LOAD_5MIN, load5);
            send_sensor_state(state->ctx, SENSOR_KEY_LOAD_15MIN, load15);

            /* Update temperature sensors */
            for (int i = 0; i < state->thermal_zone_count; i++) {
                float temp = read_temperature(state->thermal_zones[i].name);
                send_sensor_state(state->ctx, state->thermal_zones[i].key, temp);
            }

            last_cpu_mem = now;
        }

        /* Network update (1 minute) */
        uint64_t elapsed_net = (now.tv_sec - last_network.tv_sec) * 1000 +
                               (now.tv_nsec - last_network.tv_nsec) / 1000000;
        if (elapsed_net >= NETWORK_UPDATE_INTERVAL_MS) {
            read_network_stats(state);
            for (int i = 0; i < state->interface_count; i++) {
                send_sensor_state(state->ctx, state->interfaces[i].key_rx,
                                 (float)state->interfaces[i].rx_bytes);
                send_sensor_state(state->ctx, state->interfaces[i].key_tx,
                                 (float)state->interfaces[i].tx_bytes);
            }
            last_network = now;
        }

        /* Disk update (5 minutes) */
        uint64_t elapsed_disk = (now.tv_sec - last_disk.tv_sec) * 1000 +
                                (now.tv_nsec - last_disk.tv_nsec) / 1000000;
        if (elapsed_disk >= DISK_UPDATE_INTERVAL_MS) {
            read_disk_mounts(state);
            for (int i = 0; i < state->disk_count; i++) {
                float disk_usage = read_disk_usage(state->disks[i].mountpoint);
                send_sensor_state(state->ctx, state->disks[i].key, disk_usage);
            }
            last_disk = now;
        }

        /* Sleep in small increments, checking shutdown pipe each time */
        {
            int ms_slept = 0;
            while (ms_slept < 1000 && state->update_thread_running) {
                struct pollfd pfd;
                pfd.fd = state->shutdown_pipe[0];
                pfd.events = POLLIN;
                int ret = poll(&pfd, 1, 100);  /* 100ms timeout */
                if (ret > 0) {
                    break;  /* Got shutdown signal */
                }
                ms_slept += 100;
            }
        }
    }

    return NULL;
}

/**
 * Register a sensor entity
 */
static void register_sensor_entity(esphome_plugin_context_t *ctx, int client_id,
                                   uint32_t key, const char *object_id,
                                   const char *name, const char *icon,
                                   const char *unit, int decimals,
                                   const char *device_class,
                                   sensor_state_class_t state_class) {
    list_entities_sensor_response_t entity = {0};
    entity.key = key;
    entity.accuracy_decimals = decimals;
    entity.state_class = state_class;

    strncpy(entity.object_id, object_id, sizeof(entity.object_id) - 1);
    strncpy(entity.name, name, sizeof(entity.name) - 1);
    if (icon) strncpy(entity.icon, icon, sizeof(entity.icon) - 1);
    if (unit) strncpy(entity.unit_of_measurement, unit, sizeof(entity.unit_of_measurement) - 1);
    if (device_class) strncpy(entity.device_class, device_class, sizeof(entity.device_class) - 1);

    uint8_t buf[512];
    size_t len = sensor_encode_list_entities_response(buf, sizeof(buf), &entity);

    if (len > 0) {
        esphome_plugin_send_message_to_client(ctx, client_id,
                                              ESPHOME_MSG_LIST_ENTITIES_SENSOR_RESPONSE,
                                              buf, len);
    }
}

/**
 * List entities callback - exposes all sensors to Home Assistant
 */
static int sensors_list_entities(esphome_plugin_context_t *ctx, int client_id) {
    sensors_state_t *state = (sensors_state_t *)ctx->plugin_data;

    if (!state) return -1;

    /* CPU and Memory sensors */
    register_sensor_entity(ctx, client_id, SENSOR_KEY_CPU_USAGE,
                          "cpu_usage", "CPU Usage", "mdi:cpu-64-bit",
                          "%", 1, "power_factor", SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_LOAD_1MIN,
                          "load_1min", "Load Average (1min)", "mdi:cpu-64-bit",
                          "", 2, NULL, SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_LOAD_5MIN,
                          "load_5min", "Load Average (5min)", "mdi:cpu-64-bit",
                          "", 2, NULL, SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_LOAD_15MIN,
                          "load_15min", "Load Average (15min)", "mdi:cpu-64-bit",
                          "", 2, NULL, SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_MEMORY_USAGE,
                          "memory_usage", "Memory Usage", "mdi:memory",
                          "%", 1, "power_factor", SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_MEMORY_TOTAL,
                          "memory_total", "Total Memory", "mdi:memory",
                          "MB", 0, "data_size", SENSOR_STATE_CLASS_MEASUREMENT);

    register_sensor_entity(ctx, client_id, SENSOR_KEY_MEMORY_AVAILABLE,
                          "memory_available", "Available Memory", "mdi:memory",
                          "MB", 0, "data_size", SENSOR_STATE_CLASS_MEASUREMENT);

    /* Temperature sensors */
    scan_thermal_zones(state);
    for (int i = 0; i < state->thermal_zone_count; i++) {
        char object_id[128], name[128];

        /* object_id: e.g. "thermal_zone0_temperature" */
        snprintf(object_id, sizeof(object_id), "%s_temperature", state->thermal_zones[i].name);
        /* name: e.g. "thermal_zone0 (cpu-thermal)" */
        snprintf(name, sizeof(name), "%s (%s)", state->thermal_zones[i].name,
                 state->thermal_zones[i].type);

        printf("[sensors] registering temp: key=%u object_id='%s' name='%s'\n",
               state->thermal_zones[i].key, object_id, name);
        register_sensor_entity(ctx, client_id, state->thermal_zones[i].key,
                              object_id, name, "mdi:thermometer",
                              "°C", 1, "temperature", SENSOR_STATE_CLASS_MEASUREMENT);
    }

    /* Network interfaces */
    read_network_stats(state);
    printf("[sensors] list_entities: registering %d network interfaces\n", state->interface_count);
    for (int i = 0; i < state->interface_count; i++) {
        char object_id[128], name[128];

        /* RX sensor */
        snprintf(object_id, sizeof(object_id), "%s_rx_bytes", state->interfaces[i].name);
        snprintf(name, sizeof(name), "%s RX", state->interfaces[i].name);
        printf("[sensors] registering RX: key=%u object_id='%s' name='%s'\n",
               state->interfaces[i].key_rx, object_id, name);
        register_sensor_entity(ctx, client_id, state->interfaces[i].key_rx,
                              object_id, name, "mdi:network",
                              "B", 0, "data_size", SENSOR_STATE_CLASS_TOTAL_INCREASING);

        /* TX sensor */
        snprintf(object_id, sizeof(object_id), "%s_tx_bytes", state->interfaces[i].name);
        snprintf(name, sizeof(name), "%s TX", state->interfaces[i].name);
        printf("[sensors] registering TX: key=%u object_id='%s' name='%s'\n",
               state->interfaces[i].key_tx, object_id, name);
        register_sensor_entity(ctx, client_id, state->interfaces[i].key_tx,
                              object_id, name, "mdi:network",
                              "B", 0, "data_size", SENSOR_STATE_CLASS_TOTAL_INCREASING);
    }

    /* Disk usage */
    read_disk_mounts(state);
    for (int i = 0; i < state->disk_count; i++) {
        char object_id[128], name[128];

        /* Create a sanitized object_id from mountpoint */
        const char *mp = state->disks[i].mountpoint;
        if (strcmp(mp, "/") == 0) {
            snprintf(object_id, sizeof(object_id), "disk_root");
            snprintf(name, sizeof(name), "Disk Root (/)");
        } else {
            /* Skip leading slash and replace remaining slashes with underscores */
            char *p = object_id;
            const char *src = mp + 1;  /* Skip leading / */
            *p++ = 'd';
            *p++ = 'i';
            *p++ = 's';
            *p++ = 'k';
            *p++ = '_';
            while (*src && p < object_id + sizeof(object_id) - 1) {
                *p++ = (*src == '/') ? '_' : *src;
                src++;
            }
            *p = '\0';

            snprintf(name, sizeof(name), "Disk Usage (%s)", mp);
        }

        register_sensor_entity(ctx, client_id, state->disks[i].key,
                              object_id, name, "mdi:harddisk",
                              "%", 1, "power_factor", SENSOR_STATE_CLASS_MEASUREMENT);
    }

    return 0;
}

/**
 * Subscribe states callback - sends initial sensor values
 */
static int sensors_subscribe_states(esphome_plugin_context_t *ctx, int client_id) {
    (void)client_id;
    sensors_state_t *state = (sensors_state_t *)ctx->plugin_data;

    if (!state) return -1;

    /* Send initial sensor values */
    update_sensors(state);

    return 0;
}

/**
 * Initialize the Sensors plugin
 */
static int sensors_init(esphome_plugin_context_t *ctx) {
    printf("[sensors] Initializing plugin\n");

    /* Allocate plugin state */
    sensors_state_t *state = calloc(1, sizeof(sensors_state_t));
    if (!state) {
        fprintf(stderr, "[sensors] Failed to allocate state\n");
        return -1;
    }

    state->ctx = ctx;
    state->interface_count = 0;
    state->disk_count = 0;
    state->entities_registered = false;

    /* Create shutdown pipe */
    if (pipe(state->shutdown_pipe) != 0) {
        fprintf(stderr, "[sensors] Failed to create shutdown pipe\n");
        free(state);
        return -1;
    }

    /* Store in context */
    ctx->plugin_data = state;

    /* Start update thread */
    state->update_thread_running = true;
    if (pthread_create(&state->update_thread, NULL, update_thread_func, state) != 0) {
        fprintf(stderr, "[sensors] Failed to create update thread\n");
        free(state);
        return -1;
    }

    printf("[sensors] Plugin initialized successfully\n");
    printf("[sensors] Device: %s\n", ctx->config->device_name);

    return 0;
}

/**
 * Cleanup the Sensors plugin
 */
static void sensors_cleanup(esphome_plugin_context_t *ctx) {
    printf("[sensors] Cleaning up plugin\n");
    fflush(stdout);

    if (ctx->plugin_data) {
        sensors_state_t *state = (sensors_state_t *)ctx->plugin_data;
        printf("[sensors] state=%p update_thread_running=%d\n", (void*)state, state->update_thread_running);
        fflush(stdout);

        /* Signal thread to stop */
        state->update_thread_running = false;

        /* Wake up thread from poll() via shutdown pipe */
        {
            char dummy = 'x';
            write(state->shutdown_pipe[1], &dummy, 1);
        }

        /* Wait for thread to exit */
        pthread_join(state->update_thread, NULL);

        /* Close pipe */
        close(state->shutdown_pipe[0]);
        close(state->shutdown_pipe[1]);

        free(state);
        ctx->plugin_data = NULL;
    }

    printf("[sensors] Cleanup complete\n");
    fflush(stdout);
}

/**
 * Message handler - not used for sensors (read-only)
 */
static int sensors_handle_message(esphome_plugin_context_t *ctx,
                                  int client_id,
                                  uint32_t msg_type,
                                  const uint8_t *data,
                                  size_t len) {
    (void)ctx;
    (void)client_id;
    (void)data;
    (void)len;

    /* Sensors plugin doesn't handle any incoming messages */
    return -1;
}

/**
 * Register the plugin
 */
ESPHOME_PLUGIN_REGISTER(sensors_plugin, "Sensors", "1.0.0",
    sensors_init,
    sensors_cleanup,
    sensors_handle_message,
    NULL,  /* configure_device_info */
    sensors_list_entities,
    sensors_subscribe_states
);
