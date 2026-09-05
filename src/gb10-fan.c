// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.2.0"
/* Mirrors TARGET_RPM_MAX in the module, which rejects anything outside
 * 1..13500 with -ERANGE. Used only to reject bad input early, before a write is
 * attempted or a curve is accepted; keep both in step if the module changes.
 */
#define MAX_RPM 13500
/* Lowest maximum among the fans' capability ranges as reported by upstream's
 * reverse-engineered EC replies (fan0 1260-9000, fan1 1890-13500). These figures
 * are UNCONFIRMED by vendor documentation and are never measured here; no
 * tachometer readback exists. Automatic policy stays at or below this value as a
 * conservative default only. It is not a verified safe limit, and choosing any
 * RPM remains the operator's responsibility. See the README disclaimer.
 */
#define RATED_MAX_RPM 9000
#define MAX_BANDS 8
/* Identity of the FF-A firmware partition that owns the EC mailbox, mirroring
 * the module's UUID_INIT device-ID entry. Checked against the device's uuid
 * attribute so eSPI frames are never sent to an unexpected partition. Verify
 * firmware compatibility instead of editing this to force a match.
 */
#define PARTITION_UUID "884a63a0-3285-4120-83aa-eec008a0a546"

/* Stable NVML C ABI, loaded optionally without a CUDA development dependency. */
typedef struct nvmlDevice_st *nvml_device;
struct temperature_source {
    void *library;
    int (*init)(void);
    int (*shutdown)(void);
    int (*count)(unsigned int *);
    int (*device)(unsigned int, nvml_device *);
    int (*temperature)(nvml_device, unsigned int, unsigned int *);
    bool initialized;
    const char *file;
};

/* Floor curve: rpms[0] applies below thresholds[0], rpms[n] at or above
 * thresholds[n - 1]. Thresholds are millidegrees C and strictly increasing;
 * RPMs are non-decreasing. A floor never restricts cooling above itself.
 */
struct curve {
    int thresholds[MAX_BANDS];
    int rpms[MAX_BANDS + 1];
    int bands;
};

static const struct curve default_curve = {
    .thresholds = {50000, 65000, 75000, 85000, 92000},
    .rpms = {2400, 3200, 5200, 8000, 8500, RATED_MAX_RPM},
    .bands = 5,
};

static volatile sig_atomic_t stopping;

static void stop_handler(int signo)
{
    (void)signo;
    stopping = 1;
}

static int parse_number(const char *text, long min, long max, long *value)
{
    char *end;
    long n;

    if (!text[0] || (!isdigit((unsigned char)text[0]) && text[0] != '-'))
        return -1;
    errno = 0;
    n = strtol(text, &end, 10);
    if (errno || *end || n < min || n > max)
        return -1;
    *value = n;
    return 0;
}

/* Copy one delimiter-terminated field and advance past the delimiter. Returns
 * the terminating delimiter, 0 at end of text, or -1 for an empty/oversized
 * field. Never modifies the input, so argv stays intact.
 */
static int scan_field(const char **cursor, char *buffer, size_t size, const char *delimiters)
{
    const char *start = *cursor;
    size_t length = 0;

    while (start[length] && !strchr(delimiters, start[length]))
        length++;
    if (length == 0 || length >= size)
        return -1;
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    *cursor = start[length] ? start + length + 1 : start + length;
    return (unsigned char)start[length];
}

/* Parse "base[,tempC:rpm]..." with 1 to MAX_BANDS pairs. Temperatures are whole
 * Celsius for legibility; the governor compares millidegrees internally.
 */
static int parse_curve(const char *spec, struct curve *out)
{
    const char *cursor = spec;
    char field[8];
    long value;
    int delimiter;

    *out = (struct curve){0};
    delimiter = scan_field(&cursor, field, sizeof(field), ",");
    if (delimiter < 0 || parse_number(field, 1, MAX_RPM, &value) < 0) {
        fprintf(stderr, "Curve: base RPM must be an integer 1..%d.\n", MAX_RPM);
        return -1;
    }
    out->rpms[0] = (int)value;
    if (delimiter == 0) {
        fprintf(stderr, "Curve: at least one tempC:rpm pair is required.\n");
        return -1;
    }
    while (out->bands < MAX_BANDS) {
        int band = out->bands;

        if (scan_field(&cursor, field, sizeof(field), ":,") != ':') {
            fprintf(stderr, "Curve: band %d must be written as tempC:rpm.\n", band + 1);
            return -1;
        }
        if (parse_number(field, 20, 110, &value) < 0) {
            fprintf(stderr, "Curve: band %d temperature must be an integer 20..110 C.\n",
                    band + 1);
            return -1;
        }
        out->thresholds[band] = (int)value * 1000;
        delimiter = scan_field(&cursor, field, sizeof(field), ",");
        if (delimiter < 0 || parse_number(field, 1, MAX_RPM, &value) < 0) {
            fprintf(stderr, "Curve: band %d RPM must be an integer 1..%d.\n",
                    band + 1, MAX_RPM);
            return -1;
        }
        out->rpms[band + 1] = (int)value;
        out->bands++;
        if (band > 0 && out->thresholds[band] <= out->thresholds[band - 1]) {
            fprintf(stderr, "Curve: temperatures must strictly increase (band %d).\n",
                    band + 1);
            return -1;
        }
        if (out->rpms[band + 1] < out->rpms[band]) {
            fprintf(stderr, "Curve: RPM must not decrease (band %d).\n", band + 1);
            return -1;
        }
        if (delimiter == 0)
            break;
    }
    if (delimiter != 0 || *cursor) {
        fprintf(stderr, "Curve: at most %d bands are supported.\n", MAX_BANDS);
        return -1;
    }
    for (int i = 0; i <= out->bands; i++) {
        if (out->rpms[i] > RATED_MAX_RPM) {
            fprintf(stderr, "Curve: %d RPM exceeds %d, the unconfirmed reported maximum "
                    "for fan0; suitability is yours to determine.\n",
                    out->rpms[i], RATED_MAX_RPM);
            break;
        }
    }
    return 0;
}

static int read_text(int directory, const char *path, char *buffer, size_t size)
{
    int fd = openat(directory, path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    int error;

    if (fd < 0)
        return -1;
    do {
        n = read(fd, buffer, size - 1);
    } while (n < 0 && errno == EINTR);
    error = errno;
    close(fd);
    if (n < 0) {
        errno = error;
        return -1;
    }
    if (n == 0 || (size_t)n == size - 1) {
        errno = EINVAL;
        return -1;
    }
    buffer[n] = '\0';
    if (memchr(buffer, '\0', (size_t)n)) {
        errno = EINVAL;
        return -1;
    }
    if (buffer[n - 1] == '\n')
        buffer[n - 1] = '\0';
    return 0;
}

static int open_controller(void)
{
    glob_t paths = {0};
    char uuid[64], value[64];
    int fd = -1;
    int result = glob("/sys/bus/arm_ffa/drivers/nvfancontrol/*/fan_fault", 0, NULL, &paths);

    if (result || paths.gl_pathc != 1) {
        fprintf(stderr, "Expected one compatible nvfancontrol device; found %zu.\n"
                "Load this release's module; the stock driver is not sufficient.\n",
                paths.gl_pathc);
        goto out;
    }
    *strrchr(paths.gl_pathv[0], '/') = '\0';
    fd = open(paths.gl_pathv[0], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || read_text(fd, "uuid", uuid, sizeof(uuid)) < 0 ||
        strcasecmp(uuid, PARTITION_UUID) ||
        read_text(fd, "fan", value, sizeof(value)) < 0 ||
        read_text(fd, "fan_cap", value, sizeof(value)) < 0) {
        fprintf(stderr, "Cannot validate fan-control device and its interfaces.\n");
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
out:
    globfree(&paths);
    return fd;
}

static int controller_fault(int fd)
{
    char buffer[64];
    long fault;

    if (read_text(fd, "fan_fault", buffer, sizeof(buffer)) < 0 ||
        parse_number(buffer, -4095, 0, &fault) < 0) {
        fprintf(stderr, "Cannot establish transport health; refusing to write.\n");
        return -EIO;
    }
    if (fault)
        fprintf(stderr, "Transport fault %ld is latched; inspect the kernel log. "
                "Do not reload the module to bypass it.\n", fault);
    return (int)fault;
}

static int lock_controller(void)
{
    struct stat st;
    int directory, fd;

    if (mkdir("/run/gb10-fan", 0755) < 0 && errno != EEXIST)
        return -1;
    directory = open("/run/gb10-fan", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return -1;
    if (fstat(directory, &st) < 0 || st.st_uid != 0 || (st.st_mode & 0022)) {
        close(directory);
        errno = EPERM;
        return -1;
    }
    fd = openat(directory, "control.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(directory);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0022)) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    /* Never unlink: waiting/new processes must all lock the same inode. */
    return fd;
}

static int write_control(int directory, const char *attribute, const char *value)
{
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "%s\n", value);

    for (int attempt = 0; attempt < 3; attempt++) {
        int fd, error;
        ssize_t written;

        if (controller_fault(directory))
            return -1;
        fd = openat(directory, attribute, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", attribute, strerror(errno));
            return -1;
        }
        /* A short/failed write may have reached the EC: never finish or replay it. */
        written = write(fd, buffer, (size_t)length);
        error = written < 0 ? errno : EIO;
        if (close(fd) < 0 && written == length) {
            written = -1;
            error = errno;
        }
        if (written == length)
            return 0;
        fprintf(stderr, "%s=%s failed: %s\n", attribute, value, strerror(error));
        if (error != EBUSY || controller_fault(directory) || attempt == 2)
            return -1;
        /* Only a pre-submission busy mailbox with no latched fault is retryable. */
        struct timespec delay = {.tv_sec = 1};
        nanosleep(&delay, NULL);
    }
    return -1;
}

static void temperature_init(struct temperature_source *source)
{
    if (source->file)
        return;
    source->library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!source->library)
        return;
    /* POSIX specifies dlsym function addresses; memcpy avoids ISO C cast warnings. */
#define LOAD_NVML(member, symbol) do { \
    void *address = dlsym(source->library, symbol); \
    _Static_assert(sizeof(source->member) == sizeof(address), "POSIX function pointer size"); \
    if (!address) goto unavailable; \
    memcpy(&source->member, &address, sizeof(address)); \
} while (0)
    LOAD_NVML(init, "nvmlInit_v2");
    LOAD_NVML(shutdown, "nvmlShutdown");
    LOAD_NVML(count, "nvmlDeviceGetCount_v2");
    LOAD_NVML(device, "nvmlDeviceGetHandleByIndex_v2");
    LOAD_NVML(temperature, "nvmlDeviceGetTemperature");
#undef LOAD_NVML
    return;
unavailable:
    dlclose(source->library);
    source->library = NULL;
}

static int read_temperature(struct temperature_source *source, const char **description)
{
    char buffer[64];
    long value;
    int hottest = INT_MIN;
    bool gpu_valid = false, thermal_valid = false;
    glob_t zones = {0};

    if (source->file) {
        *description = source->file;
        if (read_text(AT_FDCWD, source->file, buffer, sizeof(buffer)) == 0 &&
            parse_number(buffer, -40000, 150000, &value) == 0)
            return (int)value;
        return INT_MIN;
    }
    if (source->library) {
        unsigned int count;
        if (!source->initialized && source->init() == 0)
            source->initialized = true;
        if (source->initialized && source->count(&count) == 0 && count > 0 && count <= 64) {
            int gpu_hottest = INT_MIN;
            gpu_valid = true;
            for (unsigned int i = 0; i < count; i++) {
                nvml_device device;
                unsigned int temp;
                if (source->device(i, &device) != 0 ||
                    source->temperature(device, 0, &temp) != 0 || temp > 150) {
                    gpu_valid = false;
                    break;
                }
                if ((int)temp * 1000 > gpu_hottest)
                    gpu_hottest = (int)temp * 1000;
            }
            if (gpu_valid)
                hottest = gpu_hottest;
        }
        if (source->initialized && !gpu_valid) {
            source->shutdown();
            source->initialized = false;
        }
    }
    if (glob("/sys/class/thermal/thermal_zone*/temp", 0, NULL, &zones) == 0) {
        for (size_t i = 0; i < zones.gl_pathc; i++) {
            if (read_text(AT_FDCWD, zones.gl_pathv[i], buffer, sizeof(buffer)) == 0 &&
                parse_number(buffer, -40000, 150000, &value) == 0) {
                thermal_valid = true;
                if (value > hottest)
                    hottest = (int)value;
            }
        }
    }
    globfree(&zones);
    *description = gpu_valid ? (thermal_valid ? "NVML + thermal zones (maximum)" : "NVML") :
                   (thermal_valid ? "thermal zones (NVML unavailable)" : "unavailable");
    return hottest;
}

static int governor(int fd, struct temperature_source *source, int poll, int hysteresis,
                    const struct curve *curve)
{
    struct sigaction action = {.sa_handler = stop_handler};
    int band = -1, current = -1;
    bool sensor_failed = false;
    const char *last_source = NULL;

    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }
    if (stopping)
        return 0;
    /* The governor owns a floor only; remove any retained cap before starting. */
    if (write_control(fd, "fan_cap", "auto") < 0)
        return 1;
    while (!stopping) {
        const char *description;
        int temp = read_temperature(source, &description);
        int next = 0;
        char target[16];

        if (!last_source || strcmp(last_source, description)) {
            fprintf(stderr, "Temperature source: %s\n", description);
            last_source = description;
        }
        if (temp == INT_MIN) {
            if (!sensor_failed)
                fprintf(stderr, "No valid temperature: requesting maximum floor until readings recover.\n");
            sensor_failed = true;
            next = curve->bands;
        } else {
            if (sensor_failed)
                fprintf(stderr, "Temperature readings recovered.\n");
            sensor_failed = false;
            while (next < curve->bands && temp >= curve->thresholds[next])
                next++;
            if (band >= 0 && next < band) {
                next = band;
                while (next > 0 && temp < curve->thresholds[next - 1] - hysteresis * 1000)
                    next--;
            }
        }
        if (stopping)
            break;
        /* Adjacent bands may share an RPM; never spend an EC exchange on a no-op. */
        if (curve->rpms[next] != current) {
            snprintf(target, sizeof(target), "%d", curve->rpms[next]);
            if (write_control(fd, "fan", target) < 0) {
                fprintf(stderr, "Governor stopped after write failure; no automatic reset/retry.\n");
                return 1;
            }
            current = curve->rpms[next];
            fprintf(stderr, "Floor acknowledged: %s\n", target);
        }
        band = next;
        struct timespec delay = {.tv_sec = poll};
        sigset_t blocked, previous;
        int wait_result = 0, wait_error;

        sigemptyset(&blocked);
        sigaddset(&blocked, SIGINT);
        sigaddset(&blocked, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0) {
            perror("sigprocmask");
            return 1;
        }
        /* Atomically unblock signals while waiting, avoiding a lost stop wakeup. */
        if (!stopping)
            wait_result = pselect(0, NULL, NULL, NULL, &delay, &previous);
        wait_error = errno;
        if (sigprocmask(SIG_SETMASK, &previous, NULL) < 0 ||
            (wait_result < 0 && wait_error != EINTR)) {
            fprintf(stderr, "Governor wait failed.\n");
            return 1;
        }
    }
    /* This process is the only writer; there is no concurrent ExecStop reset. */
    if (write_control(fd, "fan", "auto") < 0) {
        fprintf(stderr, "Shutdown could not restore automatic control.\n");
        return 1;
    }
    fprintf(stderr, "Automatic control restored.\n");
    return 0;
}

/* A band narrower than the hysteresis still works, but resists stepping down. */
static void warn_sticky_bands(const struct curve *curve, int hysteresis)
{
    for (int i = 1; i < curve->bands; i++) {
        int gap = curve->thresholds[i] - curve->thresholds[i - 1];

        if (gap <= hysteresis * 1000) {
            fprintf(stderr, "Curve: a band spans %d C, at or below the %d C hysteresis; "
                    "downward transitions will be sticky.\n", gap / 1000, hysteresis);
            return;
        }
    }
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "gb10-fan " VERSION "\n"
        "Usage: gb10-fan [options] <command>\n"
        "  status           Show cached command state, transport health, temperature\n"
        "  set <0..13500>   Set floor RPM; 0 disables only the floor\n"
        "  max              Clear cap, then request a 9000 RPM floor\n"
        "  auto             Disable both cap and floor\n"
        "  cap <1..13500>   Experimental cap (requires --allow-cap and module opt-in)\n"
        "  cap off          Disable cap; no opt-in required\n"
        "  governor         Foreground controller; SIGINT/SIGTERM restores auto\n"
        "Options:\n"
        "  --poll <1..60>        Governor interval in seconds (default 5)\n"
        "  --hysteresis <0..10>  Downward hysteresis in Celsius (default 3)\n"
        "  --curve <spec>        Floor curve base,tempC:rpm,... (up to 8 bands)\n"
        "  --temp-file <path>    Read only this file, in millidegrees Celsius\n"
        "  --allow-cap           Explicitly permit an experimental cap\n"
        "  --help, --version\n");
}

int main(int argc, char **argv)
{
    enum {OPT_POLL = 256, OPT_HYSTERESIS, OPT_TEMP_FILE, OPT_CURVE, OPT_CAP, OPT_VERSION};
    static const struct option options[] = {
        {"poll", required_argument, NULL, OPT_POLL},
        {"hysteresis", required_argument, NULL, OPT_HYSTERESIS},
        {"temp-file", required_argument, NULL, OPT_TEMP_FILE},
        {"curve", required_argument, NULL, OPT_CURVE},
        {"allow-cap", no_argument, NULL, OPT_CAP},
        {"version", no_argument, NULL, OPT_VERSION},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    struct temperature_source source = {0};
    struct curve curve = default_curve;
    long poll = 5, hysteresis = 3, rpm = 0;
    bool allow_cap = false, governor_options = false;
    int option, fd, lock = -1, result = 1;
    const char *command, *argument = NULL;

    while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
        switch (option) {
        case OPT_POLL:
            if (parse_number(optarg, 1, 60, &poll) < 0)
                goto invalid;
            governor_options = true;
            break;
        case OPT_HYSTERESIS:
            if (parse_number(optarg, 0, 10, &hysteresis) < 0)
                goto invalid;
            governor_options = true;
            break;
        case OPT_CURVE: {
            /* Commit only a fully valid curve; reject before any privilege
             * check or controller access.
             */
            struct curve parsed;

            if (parse_curve(optarg, &parsed) < 0)
                return 2;
            curve = parsed;
            governor_options = true;
            break;
        }
        case OPT_TEMP_FILE:
            if (optarg[0] != '/')
                goto invalid;
            source.file = optarg;
            break;
        case OPT_CAP: allow_cap = true; break;
        case OPT_VERSION: puts(VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: goto invalid;
        }
    }
    command = optind < argc ? argv[optind++] : "status";
    if (optind < argc)
        argument = argv[optind++];
    if (optind != argc || (governor_options && strcmp(command, "governor")) ||
        (allow_cap && strcmp(command, "cap")) ||
        (source.file && strcmp(command, "governor") && strcmp(command, "status")))
        goto invalid;
    if (!strcmp(command, "governor"))
        warn_sticky_bands(&curve, (int)hysteresis);
    if (!strcmp(command, "set") || !strcmp(command, "cap")) {
        if (!argument)
            goto invalid;
        if (!strcmp(command, "cap") && !strcmp(argument, "off")) {
            rpm = 0;
        } else if (parse_number(argument, !strcmp(command, "cap") ? 1 : 0, MAX_RPM, &rpm) < 0) {
            goto invalid;
        }
        if (!strcmp(command, "cap") && rpm && !allow_cap) {
            fprintf(stderr, "Caps may restrict cooling. Numeric caps require --allow-cap "
                    "and module parameter allow_caps=1.\n");
            return 2;
        }
    } else if (argument || (strcmp(command, "status") && strcmp(command, "max") &&
                           strcmp(command, "auto") && strcmp(command, "governor"))) {
        goto invalid;
    }
    if (strcmp(command, "status")) {
        if (geteuid() != 0) {
            fprintf(stderr, "Control commands require root.\n");
            return 1;
        }
        lock = lock_controller();
        if (lock < 0) {
            fprintf(stderr, "Cannot lock controller: %s. Stop any running gb10-fan governor first.\n",
                    strerror(errno));
            return 1;
        }
    }
    fd = open_controller();
    if (fd < 0)
        goto out_lock;
    if (!strcmp(command, "status") || !strcmp(command, "governor"))
        temperature_init(&source);
    if (!strcmp(command, "status")) {
        static const char *attributes[] = {"fan", "fan_cap", "fan_fault"};
        char buffer[64];
        const char *description;
        int temp = read_temperature(&source, &description);

        puts("Last acknowledged commands (not measured fan RPM):");
        result = 0;
        for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
            if (read_text(fd, attributes[i], buffer, sizeof(buffer)) < 0) {
                fprintf(stderr, "%s: %s\n", attributes[i], strerror(errno));
                result = 1;
            } else {
                printf("  %s: %s\n", attributes[i], buffer);
                if (!strcmp(buffer, "unknown") || !strncmp(buffer, "error ", 6) ||
                    (i == 2 && strcmp(buffer, "0")))
                    result = 1;
            }
        }
        if (temp == INT_MIN) {
            puts("Temperature: unavailable");
            result = 1;
        } else {
            printf("Temperature: %.1f C [%s]\n", temp / 1000.0, description);
        }
    } else if (!strcmp(command, "governor")) {
        result = governor(fd, &source, (int)poll, (int)hysteresis, &curve);
    } else if (!strcmp(command, "auto")) {
        result = write_control(fd, "fan", "auto") < 0;
    } else if (!strcmp(command, "max")) {
        char target[16];

        snprintf(target, sizeof(target), "%d", RATED_MAX_RPM);
        result = write_control(fd, "fan_cap", "auto") < 0 ||
                 write_control(fd, "fan", target) < 0;
    } else {
        char target[16];

        snprintf(target, sizeof(target), "%ld", rpm);
        result = write_control(fd, !strcmp(command, "cap") ? "fan_cap" : "fan",
                               rpm ? target : "off") < 0;
    }
    if (strcmp(command, "status") && strcmp(command, "governor") && result == 0)
        puts("Command acknowledged.");
    if (source.initialized)
        source.shutdown();
    if (source.library)
        dlclose(source.library);
    close(fd);
out_lock:
    if (lock >= 0)
        close(lock);
    return result;
invalid:
    fprintf(stderr, "Invalid command or argument.\n");
    usage(stderr);
    return 2;
}
