/* lineproto.h -- parser for the middleware's record format.
 *
 * Records separated by 0x0A, fields by 0x1F. The middleware guarantees
 * neither byte appears inside a field (see sanitize() there), so parsing is
 * two splits and no escaping.
 *
 * Vita-header-free so it can be tested on the host against real middleware
 * output. Destructive: it writes NULs into buf.
 */
#ifndef VS_LINEPROTO_H
#define VS_LINEPROTO_H

#include <string.h>

#define VS_MAX_FIELDS 8

static void for_each_record(char *buf,
                            void (*cb)(char **fields, int nfields))
{
    char *line, *save_line = NULL;

    for (line = strtok_r(buf, "\n", &save_line); line;
         line = strtok_r(NULL, "\n", &save_line)) {
        char *fields[VS_MAX_FIELDS];
        int   n = 0;
        char *p = line;

        while (n < VS_MAX_FIELDS) {
            char *sep = strchr(p, '\x1f');
            fields[n++] = p;
            if (!sep) break;
            *sep = 0;
            p = sep + 1;
        }
        cb(fields, n);
    }
}

#endif
