#!/usr/bin/env python3
"""Download checksum-pinned official Linux tools without root privileges."""
import argparse
import hashlib
import io
import pathlib
import platform
import urllib.request
import zipfile

VERSION = "v58.2"
ASSETS = {
    "x86_64": ("linux-amd64", "32c739f71b2d39721afd294c0b038f2499a44a71b5b6bcdd85b83faca0b240b9"),
    "aarch64": ("linux-arm64", "a82bf4111a340a7ea8577bcfd62e014e8e81b9e6a35a3190f5415fb800051ab0"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / ".deps/bin")
    parser.add_argument("--with-analysis", action="store_true", help="also download trace_processor_shell for integration tests")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ASSETS:
        parser.error("run on an x86_64 or aarch64 Linux target")
    asset, expected = ASSETS[platform.machine()]
    names = ["tracebox"] + (["trace_processor_shell"] if args.with_analysis else [])
    if any((args.output_dir / name).exists() for name in names):
        parser.error("output already exists; choose an empty output directory")
    url = f"https://github.com/google/perfetto/releases/download/{VERSION}/{asset}.zip"
    print(f"Downloading {url}", flush=True)
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError("archive checksum mismatch")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name in names:
            path = args.output_dir / name
            with path.open("xb") as output:
                output.write(archive.read(f"{asset}/{name}"))
            path.chmod(0o755)
            print(path.resolve())


if __name__ == "__main__":
    main()
