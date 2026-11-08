// Interface language. Strings are written in English in the code and wrapped in T(); with Portuguese
// selected (the default), T() returns the translation from i18n.c, else the English text itself.
#pragma once

void fh1_i18n_init(void);                 // reads the setting ("ui"/"lang": "pt" or "en")
const char *fh1_lang(void);
void fh1_set_lang(const char *lang);      // saved; windows opened afterwards use it
const char *fh1_tr(const char *en);
#define T(s) fh1_tr(s)
