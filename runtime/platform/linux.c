// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) == (void *)(uintptr_t)lo;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}
void plat_write_tracking_enable(uint64_t lo, uint64_t hi) { (void)lo; (void)hi; }
void plat_write_prepare(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// Started from a desktop (the AppImage) there is no terminal: stdout / stderr go to botwrecomp.log in the data folder
// (runtime/paths.h; rt_paths_init runs later, so the same rule is applied here). Processes started by this one (the
// start screen re-executing itself for the game) append to the same log.
static char g_log_path[1024];

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    const char *env = getenv("LARECOMP_DATA_ROOT"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;  // dev mode: wherever stderr goes already
    else if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/botwrecomp", xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/botwrecomp", home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/botwrecomp.log", dir);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: BOTW Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    log_to_file_without_terminal();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe botwrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "BOTW Recomp stopped because of an error.\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BOTW Recomp", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (botwrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }

// ---- device state for the performance overlay (ui/perf_overlay.mm). Android reads it from the activity; here from
// sysfs and /proc: the CPU clusters' clocks, the process's CPU time, its memory and the system's, a thermal zone and
// the battery. -1 / 0 where a file is missing, which the overlay shows as "--".
static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

void plat_device_stats(PlatDeviceStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->cpu_pct = -1;
    out->soc_temp_c = -1;
    out->batt_temp_c = -1, out->batt_pct = -1, out->watts = -1;
    out->thermal = -1;
    out->headroom = -1;
    // the CPU clusters (one cpufreq policy each), kHz -> MHz
    for (int i = 0; i < 8; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", i);
        long khz = read_long(p);
        if (khz < 0) break;
        out->cluster_mhz[out->ncluster++] = (uint32_t)(khz / 1000);
    }
    // this process's CPU time as a share of all cores
    {
        static uint64_t last_ns, last_cpu;
        uint64_t now = plat_now_ns(), cpu = 0;
        FILE *f = fopen("/proc/self/stat", "r");
        if (f) {
            char buf[512];
            if (fgets(buf, sizeof buf, f)) {
                char *p = strrchr(buf, ')');
                if (p) {
                    unsigned long long ut = 0, st = 0;
                    if (sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
                        cpu = (ut + st) * 10000000ull;  // (USER_HZ 100)
                }
            }
            fclose(f);
        }
        if (last_ns && now > last_ns && cpu >= last_cpu) {
            double cores = (double)sysconf(_SC_NPROCESSORS_ONLN);
            double pct = (double)(cpu - last_cpu) / (double)(now - last_ns) * 100.0;  // (share of one core)
            if (cores > 0) pct /= cores;                                             // (of all of them, as the struct says)
            out->cpu_pct = (float)pct;
        }
        last_ns = now, last_cpu = cpu;
    }
    // memory: this process, and the system's available
    {
        FILE *f = fopen("/proc/self/statm", "r");
        long size = -1, res = -1;
        if (f) { if (fscanf(f, "%ld %ld", &size, &res) != 2) res = -1; fclose(f); }
        if (res >= 0) out->rss_mb = (uint32_t)((uint64_t)res * (uint64_t)sysconf(_SC_PAGESIZE) >> 20);
        f = fopen("/proc/meminfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "MemAvailable:", 13)) { out->avail_mb = (uint32_t)(atol(line + 13) >> 10); break; }
            fclose(f);
        }
    }
    // the warmest thermal zone (the game is what heats the SoC)
    for (int i = 0; i < 32; i++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
        long milli = read_long(p);
        if (milli < 0) continue;
        float c = (float)milli / 1000.0f;
        if (c > out->soc_temp_c) out->soc_temp_c = c;
    }
    // the battery
    {
        long t = read_long("/sys/class/power_supply/battery/temp");  // tenths of a degree
        if (t > 0) out->batt_temp_c = (float)t / 10.0f;
        long cap = read_long("/sys/class/power_supply/battery/capacity");
        if (cap >= 0) out->batt_pct = (float)cap;
        long v = read_long("/sys/class/power_supply/battery/voltage_now");   // uV
        long i2 = read_long("/sys/class/power_supply/battery/current_now");  // uA (may be negative)
        if (v > 0 && i2 != 0) out->watts = (float)((double)v * (double)(i2 < 0 ? -i2 : i2) / 1e12);
        long online = read_long("/sys/class/power_supply/AC/online");
        if (online < 0) online = read_long("/sys/class/power_supply/usb/online");
        out->plugged = online > 0 ? 1 : 0;
    }
    // the frame rate's own thermal ramp is not Android's: leave the cap off
    out->thermal = 0;
    (void)0;
}
void plat_frame_work(uint64_t work_ns, uint64_t target_ns) { (void)work_ns; (void)target_ns; }
void plat_frame_work_gpu(uint64_t cpu_ns, uint64_t gpu_ns, uint64_t target_ns, uint64_t game_ns) {
    (void)cpu_ns; (void)gpu_ns; (void)target_ns; (void)game_ns;
}
void plat_adpf_stats(PlatAdpfStats *out) { if (out) { memset(out, 0, sizeof *out); out->state = -1; } }
void plat_thread_unpin(void) {}

// ---- the frame-generation status the overlay asks for (vulkan/lsfg is not built for the desktop port)
int vk_fg_status(const char **why) { if (why) *why = "not built for this build"; return -1; }

// ---- driver/UI queries the Android build answers in vulkan/device.c and ui/launcher.mm (Android-gated there). These
// placeholders let the desktop build link; a real port reads them from g_vk.props and the swapchain.
bool vk_driver_is_qualcomm(void) { return false; }
bool vk_driver_is_turnip(void) { return false; }
bool vk_turnip_path_on(int setting) { return setting != 0; }
int vk_between_fps(void) { return 60; }
const char *vk_gpu_desc(void) { return "Mali-G610 MC4 - panvk (Mesa)"; }
const char *vk_driver_desc(void) { return "panvk (Mesa 26.0.8)"; }
double vk_display_hz(void) { return 60.0; }
bool touch_hud_top(void) { return false; }
const char *launcher_app_version(void) { return "0.4.4-beta (linux)"; }

#include <vulkan/vulkan.h>

// ---- the exported-file guest heap (nvn/vulkan/exec.c vk_heap_place): what the Android build gets from device.c's
// Android-only helpers and from volk's loaded entry points. The loader exports only core 1.0, so the extension entry
// points are fetched per device here; the driver queries say "not a Turnip / RADV".
bool vk_driver_is_radv(void) { return false; }
bool vk_turnip_heap_on(int setting) { (void)setting; return false; }

static PFN_vkGetMemoryFdKHR p_get_memory_fd;
static PFN_vkMapMemory2KHR p_map_memory2;

VkResult vkGetMemoryFdKHR(VkDevice dev, const VkMemoryGetFdInfoKHR *info, int *fd) {
    if (!p_get_memory_fd) p_get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    if (!p_get_memory_fd) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return p_get_memory_fd(dev, info, fd);
}

VkResult vkMapMemory2KHR(VkDevice dev, const VkMemoryMapInfoKHR *info, void **data) {
    if (!p_map_memory2) p_map_memory2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(dev, "vkMapMemory2KHR");
    if (!p_map_memory2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    return p_map_memory2(dev, info, data);
}

// Android-gated in vulkan/device.c; the desktop build references it from the heap/feedback paths.
bool vk_adreno_8xx(const char *name) { (void)name; return false; }
