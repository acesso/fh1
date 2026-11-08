#include <stdio.h>
#include <string.h>
#include "fh1_text.h"

void fh1_title_from_filename(const char *file, char *out, size_t len)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    size_t o = 0;
    int depth = 0;
    for (const char *p = base; *p && o + 1 < len; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if ((*p == ')' || *p == ']') && depth) depth--;
        else if (*p == '.' && strchr(p + 1, '.') == NULL) break;      // the extension
        else if (!depth) out[o++] = (*p == '_') ? ' ' : *p;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (!o) snprintf(out, len, "%s", base);
}
