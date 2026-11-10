#!/usr/bin/env python3
"""Checks a packaged app's ELF against what the firmware exports: every symbol the app needs must be in the SDK's
table (components/fh1_apps/run.c) or the ELF loader's C library table. The link does not catch a missing one
(an app links as a shared object); the FH1 refuses it only when it loads ("Can't find symbol").

  elfcheck.py build/NAME.app.elf      (needs ESP-IDF's environment: xtensa-esp32s3-elf-nm)
"""
import os
import re
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
fw = os.path.dirname(here)
sources = [os.path.join(fw, "components/fh1_apps/run.c"),
           os.path.join(fw, "managed_components/espressif__elf_loader/src/esp_elf_symbol.c")]
exported = set()
for s in sources:
    exported |= set(re.findall(r"ESP_ELFSYM_EXPORT\(\s*([A-Za-z_][A-Za-z_0-9]*)", open(s).read()))
out = subprocess.run(["xtensa-esp32s3-elf-nm", "-D", sys.argv[1]], capture_output=True, text=True, check=True).stdout
needed = [l.split()[-1] for l in out.splitlines() if l.split()[:1] == ["U"] or (len(l.split()) == 2 and l.split()[0] == "U")]
missing = sorted(n for n in needed if n not in exported)
if missing:
    sys.exit(f"{sys.argv[1]}: needs symbols the firmware does not export: {' '.join(missing)}")
print(f"{sys.argv[1]}: all {len(needed)} symbols it needs are exported")
