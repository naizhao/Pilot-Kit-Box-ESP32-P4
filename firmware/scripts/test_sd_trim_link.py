#!/usr/bin/env python3
"""Link the production SD erase guard with the installed RISC-V toolchain.

This fixture reproduces libmain-before-libfatfs archive extraction, then checks
the actual ELF call target. It does not execute RISC-V code or access a card.
The C host test separately verifies the wrapper's no-op behavior.
"""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WRAP = "-Wl,--wrap=sdmmc_erase_sectors"
FORCE = "-Wl,-u,__wrap_sdmmc_erase_sectors"


def link_options(source: str) -> list[str]:
    # Preserve quoted literals, but never let a commented example supply a flag.
    code = re.sub(r'"(?:\\.|[^"\\])*"|#\[[=]*\[.*?\][=]*\]|#[^\n]*',
                  lambda m: m[0] if m[0].startswith('"') else "",
                  source, flags=re.S)
    options = []
    for block in re.findall(r"target_link_options\((.*?)\)", code, re.S):
        options.extend(flag for flag in re.findall(r'"([^"\n]*)"', block)
                       if flag in (WRAP, FORCE))
    return options


def toolchain() -> Path:
    tools_root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    candidates = sorted((tools_root / "tools/riscv32-esp-elf").glob(
        "*/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"))
    if not candidates:
        raise AssertionError(f"Installed RISC-V toolchain not found under {tools_root}")
    return candidates[-1].parent


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


class SdTrimLinkTest(unittest.TestCase):
    def link(self, cmake: str) -> tuple[int, str]:
        tools = toolchain()
        gcc, ar, nm, objdump = (str(tools / f"riscv32-esp-elf-{name}")
                               for name in ("gcc", "ar", "nm", "objdump"))
        flags = ["-march=rv32imac", "-mabi=ilp32", "-O0",
                 "-ffunction-sections", "-fdata-sections",
                 "-Wall", "-Wextra", "-Werror", "-Dsdmmc_erase_arg_t=int",
                 "-I", str(ROOT / "test/record_stubs"),
                 "-I", str(ROOT / "test/host_stubs"),
                 "-I", str(ROOT / "main")]
        with tempfile.TemporaryDirectory(prefix="pk-sd-trim-link-") as directory:
            path = Path(directory)
            sources = {
                "entry": """
extern int fatfs_trim(void);
volatile int result;
void _start(void) { result = fatfs_trim(); for (;;) {} }
""",
                "fatfs": """
#include <stddef.h>
#include "sdmmc_cmd.h"
extern esp_err_t sdmmc_erase_sectors(sdmmc_card_t *, size_t, size_t, sdmmc_erase_arg_t);
int fatfs_trim(void) { return sdmmc_erase_sectors(0, 17, 3, 0); }
""",
                "sdmmc": """
#include <stddef.h>
#include "sdmmc_cmd.h"
esp_err_t sdmmc_erase_sectors(sdmmc_card_t *card, size_t start, size_t count,
                             sdmmc_erase_arg_t arg) {
    (void)card; (void)start; (void)count; (void)arg; return ESP_FAIL;
}
""",
            }
            objects = {}
            for name, content in sources.items():
                source = path / f"{name}.c"
                source.write_text(content)
                objects[name] = path / f"{name}.o"
                built = run([gcc, *flags, "-c", str(source), "-o", str(objects[name])])
                self.assertEqual(built.returncode, 0, built.stderr)
            guard = path / "guard.o"
            built = run([gcc, *flags, "-c", str(ROOT / "main/pk_sd_trim_guard.c"),
                         "-o", str(guard)])
            self.assertEqual(built.returncode, 0, built.stderr)
            for name, obj in (("main", guard), ("fatfs", objects["fatfs"]),
                              ("sdmmc", objects["sdmmc"])):
                archived = run([ar, "rcs", str(path / f"lib{name}.a"), str(obj)])
                self.assertEqual(archived.returncode, 0, archived.stderr)
            elf = path / "firmware.elf"
            linked = run([gcc, "-march=rv32imac", "-mabi=ilp32", "-nostdlib",
                          "-Wl,--gc-sections", "-Wl,--entry=_start",
                          *link_options(cmake), str(objects["entry"]),
                          str(path / "libmain.a"), str(path / "libfatfs.a"),
                          str(path / "libsdmmc.a"), "-o", str(elf)])
            if linked.returncode:
                return linked.returncode, linked.stderr
            symbols = run([nm, "--defined-only", str(elf)])
            calls = run([objdump, "-d", "--disassemble=fatfs_trim", str(elf)])
            self.assertEqual(symbols.returncode, 0, symbols.stderr)
            self.assertEqual(calls.returncode, 0, calls.stderr)
            names = {line.split()[-1] for line in symbols.stdout.splitlines() if line.split()}
            if "sdmmc_erase_sectors" in names:
                return 1, "Physical erase implementation survived garbage collection"
            if "__wrap_sdmmc_erase_sectors" not in names:
                return 1, "Production wrapper absent from ELF"
            if "<__wrap_sdmmc_erase_sectors>" not in calls.stdout:
                return 1, "FatFS consumer does not call the wrapper: " + calls.stdout
            return 0, calls.stdout

    def test_current_cmake_routes_consumer_to_guard_and_discards_real_erase(self):
        result, evidence = self.link((ROOT / "main/CMakeLists.txt").read_text())
        self.assertEqual(result, 0, evidence)

    def test_each_missing_link_option_is_detected(self):
        original = (ROOT / "main/CMakeLists.txt").read_text()
        for option in (FORCE, WRAP):
            with self.subTest(removed=option):
                self.assertEqual(link_options(original).count(option), 1)
                # Mutate a temporary CMake string, not production code. Retain
                # a comment containing the flag to reject the former grep test.
                quoted = f'"{option}"'
                self.assertEqual(original.count(quoted), 1)
                mutated = original.replace(quoted, "", 1) + f"\n# {quoted}\n"
                result, evidence = self.link(mutated)
                self.assertNotEqual(result, 0, "Link-option mutation survived")
                if option == FORCE:
                    self.assertIn("__wrap_sdmmc_erase_sectors", evidence)
                else:
                    self.assertIn("Physical erase implementation survived", evidence)


if __name__ == "__main__":
    unittest.main()
