/* Storage on libxenon: the first USB stick (ud*) or other FAT device. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <diskio/disc_io.h>
#include <sys/iosupport.h>

/* Defined in libxenon's newlib glue but not declared in any installed header. */
int bdev_enum(int handle, const char **name);

static char dir[64];

bool storage_init(void)
{
    const char *name;
    char fallback[32] = "";
    int handle = -1;

    /* Prefer a USB stick (ud*), otherwise take the first mounted device. */
    printf("Drives found:");
    while ((handle = bdev_enum(handle, &name)) >= 0) {
        printf(" %s", name);
        if (strncmp(name, "ud", 2) == 0) {
            snprintf(dir, sizeof(dir), "%s:/moonlight", name);
            break;
        }
        if (fallback[0] == '\0')
            snprintf(fallback, sizeof(fallback), "%s", name);
    }
    printf("%s\n", (dir[0] || fallback[0]) ? "" : " (none)");
    if (dir[0] == '\0' && fallback[0] != '\0')
        snprintf(dir, sizeof(dir), "%s:/moonlight", fallback);
    if (dir[0] == '\0')
        return false;

    mkdir(dir, 0777);
    return true;
}

const char *storage_dir(void)
{
    return dir[0] ? dir : NULL;
}

char *storage_read(const char *name, size_t *len)
{
    char path[128];
    FILE *f;
    char *buf;
    long size;

    if (!dir[0])
        return NULL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(size + 1);
    if (buf && fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[size] = '\0';
        if (len)
            *len = (size_t)size;
    }
    return buf;
}

bool storage_write(const char *name, const void *data, size_t len)
{
    char path[128];
    FILE *f;
    bool ok;

    if (!dir[0])
        return false;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL)
        return false;
    ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}
