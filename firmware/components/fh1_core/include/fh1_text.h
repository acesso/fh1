// Small text helpers shared by apps.
#pragma once
#include <stddef.h>

// A display title from a file name: "Metroid (Europe) (Virtual Console).nes" -> "Metroid" (no folder, no
// extension, no tags in parentheses or brackets, underscores as spaces).
void fh1_title_from_filename(const char *file, char *out, size_t len);
