// BlueWake Linux entry shim: windows/src/win_entry.c for Linux.
//
// Fills in default paths beside the executable and in the player's data
// folder, starts the session log, then runs the unchanged host
// (runtime/host/src/main.c, compiled with main renamed to
// bluewake_host_main). Rendering is Aurora through Dawn, which uses Vulkan
// on Linux.
//
// App folder (scripts/linux/build.py writes it): BlueWake and its runtime
// libraries, gGZLE01_recomp.so, game/ (GZLE01.iso, main.dol, rels/),
// initial_pipeline_cache.db. Player data ($XDG_DATA_HOME/BlueWake, by
// default ~/.local/share/BlueWake; BLUEWAKE_DATA_DIR overrides): the memory
// card, SRAM, settings.ini, disc.txt, game/, logs/, states/. An environment
// variable that is already set wins over every default here.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <aurora/aurora.h>

#include "win_disc.h"
#include "win_settings.h"

int bluewake_host_main(int argc, char** argv);

static char g_exe_dir[PATH_MAX];
static char g_data_dir[PATH_MAX];
static char g_log_path[PATH_MAX];

static int file_exists(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int dir_exists(const char* path) {
    struct stat st;
    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void bw_default(const char* name, const char* value) {
    const char* existing = getenv(name);
    if (existing != NULL && existing[0] != '\0')
        return;
    setenv(name, value, 1);
}

static void bw_default_path(const char* name, const char* dir, const char* relative) {
    char path[PATH_MAX * 2];
    snprintf(path, sizeof path, "%s%s", dir, relative);
    bw_default(name, path);
}

static void make_dirs(const char* path) {
    char partial[PATH_MAX];
    snprintf(partial, sizeof partial, "%s", path);
    for (char* p = partial + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(partial, 0755);
            *p = '/';
        }
    }
    mkdir(partial, 0755);
}

static void resolve_dirs(void) {
    ssize_t n = readlink("/proc/self/exe", g_exe_dir, sizeof g_exe_dir - 1);
    if (n > 0) {
        g_exe_dir[n] = '\0';
        char* slash = strrchr(g_exe_dir, '/');
        if (slash != NULL)
            slash[1] = '\0';
    } else {
        snprintf(g_exe_dir, sizeof g_exe_dir, "./");
    }
    const char* override = getenv("BLUEWAKE_DATA_DIR");
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    if (override != NULL && override[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s/", override);
    else if (xdg != NULL && xdg[0] == '/')
        snprintf(g_data_dir, sizeof g_data_dir, "%s/BlueWake/", xdg);
    else if (home != NULL && home[0] != '\0')
        snprintf(g_data_dir, sizeof g_data_dir, "%s/.local/share/BlueWake/", home);
    else
        snprintf(g_data_dir, sizeof g_data_dir, "%suser/", g_exe_dir);
    make_dirs(g_data_dir);
}

// Session log: stdout and stderr go to DATA/logs/session-*.log (each line
// stamped with the time) and still to the terminal.
static int g_console = -1;
static FILE* g_log_file;
static pthread_t g_log_thread;
static int g_log_running;

static void* log_pump(void* arg) {
    const int read_fd = (int)(intptr_t)arg;
    char buffer[16384];
    char line[8192];
    size_t line_len = 0;
    for (;;) {
        const ssize_t n = read(read_fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (g_console >= 0) {
            ssize_t off = 0;
            while (off < n) {
                const ssize_t w = write(g_console, buffer + off, (size_t)(n - off));
                if (w <= 0)
                    break;
                off += w;
            }
        }
        for (ssize_t i = 0; i < n; i++) {
            if (line_len < sizeof line - 1)
                line[line_len++] = buffer[i];
            if (buffer[i] != '\n')
                continue;
            line[line_len] = '\0';
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct tm local;
            localtime_r(&tv.tv_sec, &local);
            fprintf(g_log_file, "%02d:%02d:%02d.%03ld %s", local.tm_hour, local.tm_min, local.tm_sec,
                    (long)(tv.tv_usec / 1000), line);
            fflush(g_log_file);
            line_len = 0;
        }
    }
    return NULL;
}

static int name_order(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

static void prune_logs(const char* dir, int keep) {
    static char names[256][64];
    int count = 0;
    DIR* d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "session-", 8) == 0 && strlen(entry->d_name) < sizeof names[0] && count < 256)
            snprintf(names[count++], sizeof names[0], "%s", entry->d_name);
    }
    closedir(d);
    qsort(names, (size_t)count, sizeof names[0], name_order);
    for (int i = 0; i + keep < count; i++) {
        char path[PATH_MAX * 2];
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        unlink(path);
    }
}

static void finish_session_log(void) {
    if (!g_log_running)
        return;
    fflush(stdout);
    fflush(stderr);
    // /dev/null in their place (late exit handlers may still print); this
    // closes the pipe's write ends so the pump finishes.
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
    }
    pthread_join(g_log_thread, NULL);
    g_log_running = 0;
    fclose(g_log_file);
}

static void start_session_log(void) {
    g_console = dup(STDERR_FILENO);
    char logs[PATH_MAX + 8];
    snprintf(logs, sizeof logs, "%slogs/", g_data_dir);
    mkdir(logs, 0755);
    prune_logs(logs, 7);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char name[64];
    strftime(name, sizeof name, "session-%Y%m%d-%H%M%S.log", &local);
    snprintf(g_log_path, sizeof g_log_path, "%s%s", logs, name);
    g_log_file = fopen(g_log_path, "w");
    int fds[2];
    if (g_log_file == NULL || pipe2(fds, O_CLOEXEC) != 0) {
        g_log_path[0] = '\0';
        return;
    }
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pthread_create(&g_log_thread, NULL, log_pump, (void*)(intptr_t)fds[0]) == 0) {
        g_log_running = 1;
        atexit(finish_session_log);
    }
}

static void fatal_box(const char* message) {
    if (getenv("BLUEWAKE_NO_DIALOG") != NULL || (g_console >= 0 && isatty(g_console)))
        return;
    char text[4096];
    if (g_log_path[0] != '\0')
        snprintf(text, sizeof text, "%s\n\nSession log:\n%s", message, g_log_path);
    else
        snprintf(text, sizeof text, "%s", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BlueWake", text, NULL);
}

// A crash leaves its signal and the calls that led there (module+offset:
// addr2line -e MODULE OFFSET names them) in the session log.
static void report_crash(int sig, siginfo_t* info, void* context) {
    (void)context;
    char line[512];
    int n = snprintf(line, sizeof line, "[crash] signal %d (%s) address %p\n", sig, strsignal(sig),
                     info != NULL ? info->si_addr : NULL);
    if (write(STDERR_FILENO, line, (size_t)n) < 0) {
    }
    void* frames[32];
    const int count = backtrace(frames, 32);
    for (int i = 0; i < count; i++) {
        Dl_info where;
        if (dladdr(frames[i], &where) && where.dli_fname != NULL) {
            const char* base = strrchr(where.dli_fname, '/');
            n = snprintf(line, sizeof line, "[crash] #%d %s+0x%lx%s%s\n", i, base ? base + 1 : where.dli_fname,
                         (unsigned long)((const char*)frames[i] - (const char*)where.dli_fbase),
                         where.dli_sname ? " " : "", where.dli_sname ? where.dli_sname : "");
        } else {
            n = snprintf(line, sizeof line, "[crash] #%d %p\n", i, frames[i]);
        }
        if (write(STDERR_FILENO, line, (size_t)n) < 0) {
        }
    }
    usleep(250000);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_report(void) {
    static char alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = report_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

// The translated code recurses deeply (the Windows build links a 64 MB
// stack); the main thread's stack grows up to RLIMIT_STACK, raised here.
static void raise_stack_limit(void) {
    const rlim_t wanted = (rlim_t)64 << 20;
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= wanted)
        return;
    limit.rlim_cur = (limit.rlim_max == RLIM_INFINITY || limit.rlim_max >= wanted) ? wanted : limit.rlim_max;
    setrlimit(RLIMIT_STACK, &limit);
}

static void usage(void) {
    fprintf(stderr,
            "usage: BlueWake [options]\n"
            "  --widescreen       16:9 (the widescreen mod: a wider camera and HUD)\n"
            "  --aspect A         4:3 (the game's own), 16:10 or 16:9\n"
            "  --smooth           60 FPS with frame interpolation (the default; F10 toggles it)\n"
            "  --120              120 FPS with frame interpolation\n"
            "  --no-smooth        the game's own 30 FPS\n"
            "  --60hz, --30hz     the experimental 60 Hz game logic on or off for this session\n"
            "  --betterww         Better Wind Waker's settings, at their defaults\n"
            "  --options LIST     change them: name,-name,...\n"
            "  --fullscreen       start in fullscreen (F11 or Alt+Enter toggles it)\n"
            "  --window WxH       the window's size\n"
            "  --scale N          render at N x 480 lines (0: the window's own pixels)\n"
            "  --fps              show the frame rate\n"
            "  --stretch          fill the window instead of keeping the game's aspect\n"
            "  --no-mouse-camera  keep the mouse out of the camera\n"
            "  --lle-audio        run the DSP's own microcode instead of the HLE ucode\n"
            "  --mods LIST        mods compiled into the module, by name\n"
            "  --disc FILE        the disc image to read\n"
            "  --module FILE      the translated game module (default gGZLE01_recomp.so)\n"
            "Keyboard: arrows D-pad, J A, K B, U X, I Y, W/A/S/D stick, H/F/T/G C-stick,\n"
            "E/R L/R, Q Z, Return START. Game controllers work too.\n"
            "F1 settings, F9 frame rate, F10 frame interpolation, F5 save state, F8 load state.\n");
}

int main(int argc, char** argv) {
    bw_settings_capture_environment();
    raise_stack_limit();
    resolve_dirs();
    const char* session_log = getenv("BLUEWAKE_SESSION_LOG");
    if (session_log == NULL || strcmp(session_log, "0") != 0)
        start_session_log();

    const char* module_arg = NULL;
    char mods[256] = "";
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const int more = i + 1 < argc;
        if (strcmp(a, "--disc") == 0 && more) {
            setenv("BLUEWAKE_DISC", argv[++i], 1);
        } else if (strcmp(a, "--module") == 0 && more) {
            module_arg = argv[++i];
        } else if (strcmp(a, "--mods") == 0 && more) {
            const size_t used = strlen(mods);
            snprintf(mods + used, sizeof mods - used, "%s%s", used ? "," : "", argv[++i]);
        } else if (strcmp(a, "--betterww") == 0) {
            const size_t used = strlen(mods);
            snprintf(mods + used, sizeof mods - used, "%sbetterww", used ? "," : "");
        } else if (strcmp(a, "--options") == 0 && more) {
            setenv("BLUEWAKE_OPTIONS", argv[++i], 1);
        } else if (strcmp(a, "--widescreen") == 0) {
            setenv("BLUEWAKE_ASPECT", "16:9", 1);
        } else if (strcmp(a, "--aspect") == 0 && more) {
            setenv("BLUEWAKE_ASPECT", argv[++i], 1);
        } else if (strcmp(a, "--smooth") == 0) {
            setenv("DOL_AURORA_FRAME_INTERP", "1", 1);
            setenv("DOL_AURORA_FRAME_INTERP_STEPS", "1", 1);
            aurora_set_frame_interp_steps(1);
            aurora_set_frame_interpolation(true);
        } else if (strcmp(a, "--120") == 0) {
            setenv("DOL_AURORA_FRAME_INTERP", "1", 1);
            setenv("DOL_AURORA_FRAME_INTERP_STEPS", "3", 1);
            aurora_set_frame_interp_steps(3);
            aurora_set_frame_interpolation(true);
        } else if (strcmp(a, "--no-smooth") == 0) {
            setenv("DOL_AURORA_FRAME_INTERP", "0", 1);
            aurora_set_frame_interpolation(false);
        } else if (strcmp(a, "--60hz") == 0) {
            setenv("BLUEWAKE_SIMULATION_60HZ", "1", 1);
        } else if (strcmp(a, "--30hz") == 0) {
            setenv("BLUEWAKE_SIMULATION_60HZ", "0", 1);
        } else if (strcmp(a, "--fullscreen") == 0) {
            setenv("DOL_AURORA_FULLSCREEN", "1", 1);
        } else if (strcmp(a, "--window") == 0 && more) {
            setenv("DOL_AURORA_WINDOW", argv[++i], 1);
        } else if (strcmp(a, "--scale") == 0 && more) {
            setenv("DOL_AURORA_RENDER_SCALE", argv[++i], 1);
        } else if (strcmp(a, "--fps") == 0) {
            setenv("DOL_AURORA_SHOW_FPS", "1", 1);
            aurora_set_fps_overlay(true);
        } else if (strcmp(a, "--stretch") == 0) {
            setenv("DOL_AURORA_ASPECT_FIT", "0", 1);
        } else if (strcmp(a, "--no-mouse-camera") == 0) {
            setenv("BLUEWAKE_MOUSE_CAMERA", "0", 1);
        } else if (strcmp(a, "--lle-audio") == 0) {
            setenv("BLUEWAKE_DSP_MODE", "lle", 1);
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "BlueWake: unknown option %s\n", a);
            usage();
            return 2;
        }
    }
    if (mods[0] != '\0')
        setenv("BLUEWAKE_MODS", mods, 1);

    bw_settings_load(g_data_dir);
    bw_settings_apply_launch();

    bw_default("BLUEWAKE_PERF_LOG", "1");
    bw_default("BLUEWAKE_WALL_PACE", "1");
    bw_default("DOL_AUDIO_NO_THROTTLE", "1");
    bw_default("BLUEWAKE_RENDERER", "aurora");
    bw_default("BLUEWAKE_CYCLE_CAP", "16384");
    bw_default("BLUEWAKE_MAX_BLOCKS", "100000000000");
    bw_default("BLUEWAKE_DSP_MODE", "hle");
    bw_default("BLUEWAKE_CLOCK", "now");
    bw_default("BLUEWAKE_OVERLAP_OBSERVATION", "0");
    bw_default("BLUEWAKE_NATIVE_MATH", "1");
    bw_default_path("BLUEWAKE_SRAM", g_data_dir, "sram.bin");
    bw_default_path("BLUEWAKE_CARD_PATH", g_data_dir, "GZLE01.card");
    bw_default_path("BLUEWAKE_STATE_DIR", g_data_dir, "states");
    bw_default("DOL_AURORA_CACHE_DIR", g_data_dir);
    const int disc = bw_disc_setup(g_exe_dir, g_data_dir);
    if (disc != 0)
        return disc < 0 ? 1 : 0;
    bw_default_path("BLUEWAKE_DOL", g_exe_dir, "game/main.dol");
    bw_default_path("BLUEWAKE_RELS_DIR", g_exe_dir, "game/rels");
    bw_default_path("BLUEWAKE_DISC", g_exe_dir, "game/GZLE01.iso");
    bw_default_path("BLUEWAKE_DSP_IROM", g_exe_dir, "dsp/dsp_rom.bin");
    bw_default_path("BLUEWAKE_DSP_COEF", g_exe_dir, "dsp/dsp_coef.bin");

    char module[PATH_MAX * 2];
    const char* module_env = getenv("BLUEWAKE_COMPOSITE");
    if (module_arg != NULL)
        snprintf(module, sizeof module, "%s", module_arg);
    else if (module_env != NULL && module_env[0] != '\0')
        snprintf(module, sizeof module, "%s", module_env);
    else
        snprintf(module, sizeof module, "%sgGZLE01_recomp.so", g_exe_dir);

    const char* missing = NULL;
    const char* missing_path = NULL;
    if (!file_exists(module)) {
        missing = "the translated game module";
        missing_path = module;
    } else if (!file_exists(getenv("BLUEWAKE_DOL"))) {
        missing = "the prepared game executable";
        missing_path = getenv("BLUEWAKE_DOL");
    } else if (!dir_exists(getenv("BLUEWAKE_RELS_DIR"))) {
        missing = "the prepared game modules";
        missing_path = getenv("BLUEWAKE_RELS_DIR");
    } else if (!file_exists(getenv("BLUEWAKE_DISC"))) {
        missing = "the disc image";
        missing_path = getenv("BLUEWAKE_DISC");
    }
    if (missing != NULL) {
        char message[PATH_MAX * 3];
        snprintf(message, sizeof message,
                 "BlueWake cannot start: %s is missing.\n\n%s\n\n"
                 "Build your own copy with python3 scripts/linux/build.py YOUR_DISC.iso",
                 missing, missing_path ? missing_path : "");
        fprintf(stderr, "[linux] %s\n", message);
        fatal_box(message);
        return 1;
    }

    fprintf(stderr, "[linux] app=%s data=%s module=%s disc=%s\n", g_exe_dir, g_data_dir, module,
            getenv("BLUEWAKE_DISC"));
    install_crash_report();
    SDL_SetAppMetadata("BlueWake", "0.1", "dev.bluewake.BlueWake");
    bw_settings_install();
    char* host_argv[3] = {argv[0], module, NULL};
    const int status = bluewake_host_main(2, host_argv);
    if (status == 0 && bw_settings_finish_restart() < 0)
        fatal_box("BlueWake could not start again. Start it yourself; the settings are saved.");
    fflush(stdout);
    fflush(stderr);
    if (status != 0) {
        char message[256];
        snprintf(message, sizeof message, "BlueWake stopped with an error (status %d).", status);
        fatal_box(message);
    }
    // Static destructors would release Dawn's Vulkan device after SDL has
    // closed the Wayland display (a crash in the driver); everything that
    // matters (saves, settings, the log) is already written.
    finish_session_log();
    _exit(status);
}
