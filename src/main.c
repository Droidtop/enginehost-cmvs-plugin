/*
 * Desktop runner for the CMVS engine.
 *
 * Takes a game folder and, for now, reports what the engine can read out of it.
 * This is the harness the engine is developed against: it runs on Linux with no
 * Android in the picture, so a format is proven here before the plugin wraps it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpz.h"

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s <game folder>\n", argv0);
}

int main(int argc, char **argv)
{
    static const char *archives[] = {
        "ps.cpz", "script.cpz", "bg.cpz", "chip.cpz", "balloon.cpz",
        "stand.cpz", "up.cpz", "event.cpz", "se.cpz", "video.cpz",
    };
    char path[4096];
    size_t i;
    int total = 0;

    if (argc < 2) { usage(argv[0]); return 2; }

    for (i = 0; i < sizeof archives / sizeof archives[0]; i++) {
        char err[256] = {0};
        cpz_archive *a;
        snprintf(path, sizeof path, "%s/data/pack/%s", argv[1], archives[i]);
        a = cpz_open(path, err, sizeof err);
        if (!a) { printf("%-12s -- %s\n", archives[i], err); continue; }
        printf("%-12s scheme=%-14s entries=%d\n", archives[i], cpz_scheme_name(a), cpz_count(a));
        {
            int n = cpz_count(a), k;
            for (k = 0; k < 2 && k < n; k++) {
                const cpz_entry *e = cpz_at(a, k);
                int size = 0;
                uint8_t *data = cpz_read(a, e, &size, err, sizeof err);
                if (!data) { printf("    %-28s -- %s\n", e->name, err); continue; }
                printf("    %-28s %8d bytes  magic=%.4s\n", e->name, size,
                       size >= 4 ? (const char *) data : "");
                free(data);
            }
        }
        total += cpz_count(a);
        cpz_close(a);
    }
    printf("\n%d entries readable in total\n", total);
    return 0;
}
