/* The runtime side of mod scripts. A Lua script of a mod calls into the runtime by opening a path under
 * /app0/dcmod/ with loadfile (the open fails, which the script ignores): the runtime sees every path the game
 * resolves, and acts on these:
 *
 *   dcmod/log/<text>    writes "Mod: <text>" to the log (the game's own print output does not reach the log)
 *
 * The game lower-cases the paths it opens, so a script writes each capital letter as '^' and the lower-case letter
 * ("^hello" is "Hello"); the log shows the capitals again. A script should replace characters other than letters,
 * digits and " .:=_,-()[]" by '?'. At most 20000 lines are written per run; a line repeated within 5 ms is one call
 * that the file system saw twice, and is written once. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>

#define MOD_PREFIX "/app0/dcmod/"
#define MAX_LINES 20000

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Called with every guest path before it is resolved; returns 1 when the path was a mod call. */
int runtime_mod_marker(const char *guest) {
    static int lines;
    static char last[512];
    static double last_ms;
    if (!guest || strncasecmp(guest, MOD_PREFIX, sizeof(MOD_PREFIX) - 1)) return 0;
    const char *call = guest + sizeof(MOD_PREFIX) - 1;
    if (!strncasecmp(call, "log/", 4)) {
        char text[512];
        size_t n = 0;
        for (const char *p = call + 4; *p && n < sizeof(text) - 1; ++p) {
            if (*p == '^' && p[1]) {
                text[n++] = (char)toupper((unsigned char)p[1]);
                ++p;
            } else {
                text[n++] = *p;
            }
        }
        text[n] = 0;
        const double t = now_ms();
        if (!strcmp(text, last) && t - last_ms < 5.0) return 1;
        snprintf(last, sizeof(last), "%s", text);
        last_ms = t;
        const int count = __atomic_add_fetch(&lines, 1, __ATOMIC_RELAXED);
        if (count <= MAX_LINES) {
            printf("Mod: %.400s\n", text);
            fflush(stdout);
        } else if (count == MAX_LINES + 1) {
            printf("Mod: the log call limit was reached, later messages are dropped\n");
            fflush(stdout);
        }
    }
    return 1;
}
