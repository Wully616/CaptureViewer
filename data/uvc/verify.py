#!/usr/bin/env python3
"""Validate a staged or installed CaptureViewer UVC module bundle."""
import configparser
import hashlib
import os
import re
import stat
import subprocess
import sys

ALLOWED_MODULES = {"uvc", "videobuf2-vmalloc", "uvcvideo"}
NAME_RE = re.compile(r"^[A-Za-z0-9@._+-]+$")
SOURCE_RE = re.compile(r"^torvalds/linux@v[0-9]+(?:\.[0-9]+){1,2}$")


def fail(message):
    print(f"invalid UVC module bundle: {message}", file=sys.stderr)
    raise SystemExit(1)


def output(program, *arguments):
    try:
        return subprocess.check_output([program, *arguments], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        fail(f"{program} failed: {error}")


def check_file(path, require_root):
    try:
        info = os.lstat(path)
    except OSError as error:
        fail(f"missing {os.path.basename(path)}: {error}")
    if not stat.S_ISREG(info.st_mode):
        fail(f"{os.path.basename(path)} is not a regular file")
    if require_root and (info.st_uid != 0 or info.st_mode & 0o022):
        fail(f"{os.path.basename(path)} is not safely root-owned")
    return info


def main():
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != "--require-root"):
        fail("usage: verify.py DIRECTORY KERNEL_RELEASE [--require-root]")
    directory, kernel = sys.argv[1:3]
    require_root = len(sys.argv) == 4
    if kernel != os.uname().release or not re.fullmatch(r"[A-Za-z0-9._+-]+", kernel):
        fail("requested kernel does not equal uname -r")
    try:
        dir_info = os.lstat(directory)
    except OSError as error:
        fail(f"bundle directory unavailable: {error}")
    if not stat.S_ISDIR(dir_info.st_mode) or os.path.realpath(directory) != directory:
        fail("bundle directory is not a real directory")
    if require_root and (dir_info.st_uid != 0 or dir_info.st_mode & 0o022):
        fail("bundle directory is not safely root-owned")

    manifest_path = os.path.join(directory, "manifest.ini")
    manifest_info = check_file(manifest_path, require_root)
    if manifest_info.st_size > 64 * 1024:
        fail("manifest is unexpectedly large")
    manifest = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        with open(manifest_path, encoding="utf-8") as file:
            manifest.read_file(file)
    except (OSError, configparser.Error) as error:
        fail(f"cannot parse manifest: {error}")
    if manifest.sections() != ["driver"]:
        fail("unexpected manifest sections")
    data = manifest["driver"]
    required = {
        "format", "kernel-release", "source", "source-files-sha256",
        "headers-package", "headers-version", "modules",
    }
    if not required.issubset(data.keys()):
        fail("manifest is missing required metadata")
    if data["format"] != "1" or data["kernel-release"] != kernel:
        fail("manifest format or kernel release mismatch")
    if not SOURCE_RE.fullmatch(data["source"]):
        fail("manifest source is not a pinned upstream Linux release tag")
    release = kernel.split("-", 1)[0]
    if release.endswith(".0"):
        release = release[:-2]
    if data["source"] != f"torvalds/linux@v{release}":
        fail("source tag does not match this kernel's base release")
    if not re.fullmatch(r"[0-9a-f]{64}", data["source-files-sha256"]):
        fail("invalid source file hash-list digest")

    source_list_path = os.path.join(directory, "source-files.sha256")
    source_list_info = check_file(source_list_path, require_root)
    if source_list_info.st_size > 1024 * 1024:
        fail("source file hash list is unexpectedly large")
    try:
        with open(source_list_path, "rb") as file:
            source_list = file.read()
        entries = source_list.decode("ascii").splitlines()
    except (OSError, UnicodeError) as error:
        fail(f"cannot read source file hash list: {error}")
    if source_list != ("\n".join(entries) + "\n").encode("ascii"):
        fail("source file hash list is not canonically encoded")
    if hashlib.sha256(source_list).hexdigest() != data["source-files-sha256"]:
        fail("source file hash list checksum mismatch")
    source_line = re.compile(r"^([A-Za-z0-9_./+-]+) ([0-9a-f]{40})$")
    paths = []
    for entry in entries:
        match = source_line.fullmatch(entry)
        path = match.group(1) if match is not None else ""
        if (
            match is None
            or path.startswith("/")
            or path.endswith("/")
            or "//" in path
            or not path.endswith((".c", ".h"))
            or any(part in (".", "..") for part in path.split("/"))
        ):
            fail("invalid source file hash list entry")
        paths.append(path)
    if (
        not entries
        or entries != sorted(entries)
        or len(set(paths)) != len(paths)
        or not any(path.startswith("drivers/media/usb/uvc/") for path in paths)
    ):
        fail("source file hash list is empty, unsorted, duplicated, or lacks UVC sources")
    if not NAME_RE.fullmatch(data["headers-package"]) or not NAME_RE.fullmatch(data["headers-version"]):
        fail("invalid kernel headers package metadata")

    modules = data["modules"].split(",")
    if not modules or len(modules) > len(ALLOWED_MODULES):
        fail("invalid module count")
    if len(set(modules)) != len(modules) or any(module not in ALLOWED_MODULES for module in modules):
        fail("unexpected or duplicate module name")
    if modules[-1] != "uvcvideo":
        fail("uvcvideo must be last in the load order")

    order_path = os.path.join(directory, "load-order")
    check_file(order_path, require_root)
    try:
        with open(order_path, encoding="ascii") as file:
            order_content = file.read()
    except (OSError, UnicodeError) as error:
        fail(f"cannot read load-order: {error}")
    load_order = order_content.splitlines()
    if order_content != ("\n".join(load_order) + "\n") or load_order != modules:
        fail("load-order is not canonical or does not match the manifest")

    expected_keys = required | {f"sha256-{module}" for module in modules} | {
        f"srcversion-{module}" for module in modules
    }
    if set(data.keys()) != expected_keys:
        fail("manifest contains unexpected or missing fields")

    for module in modules:
        path = os.path.join(directory, f"{module}.ko")
        module_info = check_file(path, require_root)
        if module_info.st_size > 64 * 1024 * 1024:
            fail(f"{module}.ko is unexpectedly large")
        digest = hashlib.sha256()
        try:
            with open(path, "rb") as file:
                for chunk in iter(lambda: file.read(1024 * 1024), b""):
                    digest.update(chunk)
        except OSError as error:
            fail(f"cannot hash {module}.ko: {error}")
        if digest.hexdigest() != data[f"sha256-{module}"]:
            fail(f"{module}.ko checksum mismatch")
        vermagic = output("modinfo", "-F", "vermagic", path).split()
        if not vermagic or vermagic[0] != kernel:
            fail(f"{module}.ko vermagic mismatch")
        name = output("modinfo", "-F", "name", path).replace("_", "-")
        if name != module:
            fail(f"{module}.ko has unexpected module name {name}")
        srcversion = output("modinfo", "-F", "srcversion", path)
        if srcversion:
            if not re.fullmatch(r"[0-9A-Fa-f]{16,64}", srcversion):
                fail(f"{module}.ko has an invalid source version")
            if srcversion != data[f"srcversion-{module}"]:
                fail(f"{module}.ko source version mismatch")
        elif data[f"srcversion-{module}"]:
            fail(f"manifest has a source version for {module}.ko, but the module has none")
        elf = output("readelf", "-h", path)
        if not re.search(r"^\s*Machine:\s+AArch64\s*$", elf, re.MULTILINE):
            fail(f"{module}.ko is not an AArch64 ELF object")

    print(f"OK kernel={kernel} source={data['source']} modules={','.join(modules)}")


if __name__ == "__main__":
    main()
