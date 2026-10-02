// BlueWake for Linux: the player's disc, the first time and whenever it goes
// missing. See windows/src/win_disc.h; this is win_disc.c's logic with
// zenity (when installed) for the file picker and the messages.
//
// An .iso or .gcm is used where it is; a Dolphin image (.rvz, .wia, .gcz,
// .ciso, .nfs) is unpacked once to DATA/GZLE01.iso with nodtool (beside the
// app or on PATH). The disc is checked and prepared once, by the iOS app's
// importer (apple/ios/src/disc_import.c), into DATA/game. disc.txt remembers
// the disc.
#define _GNU_SOURCE
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "disc_import.h"
#include "win_disc.h"

extern char** environ;

#define BW_PATH 4096

static const char* g_exe;
static const char* g_data;

static int is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int have_zenity(void) {
    if (getenv("BLUEWAKE_NO_DIALOG") != NULL || getenv("BLUEWAKE_DISC_CHOICE") != NULL)
        return 0;
    if (getenv("WAYLAND_DISPLAY") == NULL && getenv("DISPLAY") == NULL)
        return 0;
    return system("command -v zenity >/dev/null 2>&1") == 0;
}

// Runs argv; its first stdout line to out (when given). Returns the exit code.
static int run(char* const argv[], char* out, size_t size) {
    int fds[2] = {-1, -1};
    if (out != NULL && pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out != NULL) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
    }
    pid_t pid;
    const int failed = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (out != NULL) {
        close(fds[1]);
        out[0] = '\0';
        if (!failed) {
            FILE* f = fdopen(fds[0], "r");
            if (f != NULL) {
                if (fgets(out, (int)size, f) == NULL)
                    out[0] = '\0';
                fclose(f);
            } else {
                close(fds[0]);
            }
            size_t n = strlen(out);
            while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
                out[--n] = '\0';
        } else {
            close(fds[0]);
        }
    }
    if (failed)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void box(const char* text, int error) {
    if (!have_zenity())
        return;
    char* argv[] = {"zenity", error ? "--error" : "--warning", "--title=BlueWake", "--no-markup", "--width=480",
                    "--text", (char*)text, NULL};
    run(argv, NULL, 0);
}

static int read_line(const char* path, char* out, size_t size) {
    out[0] = '\0';
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    if (fgets(out, (int)size, f) == NULL)
        out[0] = '\0';
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return n > 0;
}

static void write_line(const char* path, const char* line) {
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void disc_stamp(const char* disc, char* out, size_t size) {
    struct stat st;
    if (stat(disc, &st) != 0) {
        snprintf(out, size, "%s", disc);
        return;
    }
    snprintf(out, size, "%s|%llu|%llu", disc, (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
}

static const char* extension(const char* path) {
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '/');
    return dot != NULL && (slash == NULL || dot > slash) ? dot : "";
}

static int needs_unpacking(const char* path) {
    static const char* const kinds[] = {".rvz", ".wia", ".gcz", ".ciso", ".nfs", ".wbfs"};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (strcasecmp(extension(path), kinds[i]) == 0)
            return 1;
    return 0;
}

static void note_progress(void* context, double fraction, const char* stage) {
    (void)context;
    static int last = -1;
    const int percent = (int)(fraction * 100.0);
    if (percent == last)
        return;
    last = percent;
    fprintf(stderr, "[disc] %3d%% %s\n", percent, stage != NULL ? stage : "");
}

static int unpack(const char* disc, char* out, size_t size, char* error, size_t error_size) {
    char tool[BW_PATH], pending[BW_PATH];
    snprintf(tool, sizeof tool, "%snodtool", g_exe);
    if (!is_file(tool))
        snprintf(tool, sizeof tool, "nodtool");
    snprintf(pending, sizeof pending, "%sGZLE01.unpacking.iso", g_data);
    unlink(pending);
    char* argv[] = {tool, "--no-color", "convert", (char*)disc, pending, NULL};
    const int code = run(argv, NULL, 0);
    if (code != 0 || !is_file(pending)) {
        unlink(pending);
        snprintf(error, error_size,
                 code == -1 || code == 127
                     ? "nodtool, which unpacks %s images, was not found (beside BlueWake or on PATH). Convert "
                       "the image to ISO in Dolphin (right-click the game, Convert File)."
                     : "nodtool could not unpack this %s image. Is it a GameCube disc image?",
                 extension(disc));
        return -1;
    }
    snprintf(out, size, "%sGZLE01.iso", g_data);
    if (rename(pending, out) != 0) {
        unlink(pending);
        snprintf(error, error_size, "The unpacked image could not be kept in %s (%s).", g_data, strerror(errno));
        return -1;
    }
    return 0;
}

static int choose(const char* why, char* out, size_t size) {
    const char* testing = getenv("BLUEWAKE_DISC_CHOICE");
    if (testing != NULL) {
        snprintf(out, size, "%s", testing);
        return testing[0] != '\0';
    }
    if (!have_zenity()) {
        fprintf(stderr,
                "[disc] %sBlueWake needs your own GameCube USA disc image (GZLE01). Start it with "
                "--disc /path/to/disc.iso (it is remembered).\n",
                why);
        return 0;
    }
    char text[2048];
    snprintf(text, sizeof text,
             "%sBlueWake plays The Legend of Zelda: The Wind Waker from your own copy of the game: the "
             "GameCube disc for the USA (GZLE01).\n\nChoose your disc image next: an .iso or .gcm file, or a "
             "Dolphin .rvz. BlueWake checks it and prepares it once, then remembers it.",
             why);
    char* ask[] = {"zenity", "--question", "--title=BlueWake: choose your disc", "--no-markup", "--width=480",
                   "--ok-label=Choose", "--cancel-label=Quit", "--text", text, NULL};
    if (run(ask, NULL, 0) != 0)
        return 0;
    char* pick[] = {"zenity",
                    "--file-selection",
                    "--title=Choose your Wind Waker disc image (GameCube, USA)",
                    "--file-filter=GameCube disc images | *.iso *.gcm *.rvz *.wia *.gcz *.ciso *.nfs *.ISO *.GCM "
                    "*.RVZ",
                    "--file-filter=All files | *",
                    NULL};
    return run(pick, out, size) == 0 && out[0] != '\0';
}

int bw_disc_setup(const char* exe_dir, const char* data_dir) {
    g_exe = exe_dir;
    g_data = data_dir;
    const char* given_dol = getenv("BLUEWAKE_DOL");
    if (given_dol != NULL && given_dol[0] != '\0')
        return 0;
    char path[BW_PATH];
    const char* given = getenv("BLUEWAKE_DISC");
    const int explicit_disc = given != NULL && given[0] != '\0';
    char app_disc[BW_PATH], app_dol[BW_PATH], app_rels[BW_PATH];
    snprintf(app_disc, sizeof app_disc, "%sgame/GZLE01.iso", exe_dir);
    snprintf(app_dol, sizeof app_dol, "%sgame/main.dol", exe_dir);
    snprintf(app_rels, sizeof app_rels, "%sgame/rels", exe_dir);
    if (!explicit_disc && is_file(app_disc) && is_file(app_dol) && is_dir(app_rels))
        return 0;
    if (explicit_disc && is_file(app_dol) && is_dir(app_rels))
        return 0;

    char remembered[BW_PATH], game[BW_PATH], dol[BW_PATH + 16], rels[BW_PATH + 16], stamp_path[BW_PATH + 16];
    snprintf(remembered, sizeof remembered, "%sdisc.txt", data_dir);
    snprintf(game, sizeof game, "%sgame", data_dir);
    snprintf(dol, sizeof dol, "%s/main.dol", game);
    snprintf(rels, sizeof rels, "%s/rels", game);
    snprintf(stamp_path, sizeof stamp_path, "%s/prepared.txt", game);
    char disc[BW_PATH] = "";
    if (explicit_disc)
        snprintf(disc, sizeof disc, "%s", given);
    else
        read_line(remembered, disc, sizeof disc);
    if (explicit_disc && !is_file(disc)) {
        char text[BW_PATH + 128];
        snprintf(text, sizeof text, "BlueWake cannot find the disc image it was given:\n\n%s", disc);
        fprintf(stderr, "[disc] %s\n", text);
        box(text, 1);
        return -1;
    }
    const char* why = disc[0] != '\0' && !is_file(disc) ? "The disc image BlueWake used before is gone.\n\n" : "";
    const int scripted = getenv("BLUEWAKE_DISC_CHOICE") != NULL;
    int unpacked = 0;
    for (;;) {
        if (disc[0] == '\0' || !is_file(disc)) {
            if (!choose(why, path, sizeof path)) {
                fprintf(stderr, "[disc] no disc image chosen\n");
                return 1;
            }
            snprintf(disc, sizeof disc, "%s", path);
            unpacked = 0;
            if (needs_unpacking(disc)) {
                char out[BW_PATH], error[1024];
                fprintf(stderr, "[disc] unpacking %s\n", disc);
                if (unpack(disc, out, sizeof out, error, sizeof error) != 0) {
                    fprintf(stderr, "[disc] %s\n", error);
                    box(error, 0);
                    if (scripted)
                        return -1;
                    disc[0] = '\0';
                    why = "";
                    continue;
                }
                snprintf(disc, sizeof disc, "%s", out);
                unpacked = 1;
            }
        }
        char error[1024] = "";
        if (bluewake_disc_check(disc, error, sizeof error) != 0) {
            char text[BW_PATH + 1200];
            snprintf(text, sizeof text,
                     "%s\n\n%s\n\nChoose an image of the GameCube USA disc (GZLE01) instead: an .iso, .gcm or "
                     "Dolphin .rvz file.",
                     disc, error);
            fprintf(stderr, "[disc] %s: %s\n", disc, error);
            box(text, 0);
            if (unpacked)
                unlink(disc);
            if (explicit_disc || scripted || !have_zenity())
                return -1;
            disc[0] = '\0';
            why = "";
            continue;
        }
        char stamp[BW_PATH + 64], prepared[BW_PATH + 64];
        disc_stamp(disc, stamp, sizeof stamp);
        read_line(stamp_path, prepared, sizeof prepared);
        if (strcmp(stamp, prepared) != 0 || !is_file(dol) || !is_dir(rels)) {
            mkdir(game, 0755);
            fprintf(stderr, "[disc] preparing %s\n", disc);
            if (bluewake_disc_prepare(disc, game, note_progress, NULL, error, sizeof error) != 0) {
                char text[BW_PATH + 1200];
                snprintf(text, sizeof text, "%s\n\n%s\n\nBlueWake needs the USA disc, revision 0 (GZLE01).", disc,
                         error);
                fprintf(stderr, "[disc] %s: %s\n", disc, error);
                box(text, 0);
                if (unpacked)
                    unlink(disc);
                if (explicit_disc || scripted || !have_zenity())
                    return -1;
                disc[0] = '\0';
                why = "";
                continue;
            }
            write_line(stamp_path, stamp);
        }
        break;
    }
    if (!explicit_disc)
        write_line(remembered, disc);
    setenv("BLUEWAKE_DISC", disc, 1);
    setenv("BLUEWAKE_DOL", dol, 1);
    setenv("BLUEWAKE_RELS_DIR", rels, 1);
    fprintf(stderr, "[disc] %s (prepared in %s)\n", disc, game);
    return 0;
}
