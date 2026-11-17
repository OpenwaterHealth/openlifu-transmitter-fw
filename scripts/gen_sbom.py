#!/usr/bin/env python3
"""
gen_sbom.py - CycloneDX 1.6 SBOM for an Open-LIFU firmware or bootloader image.

The same file lives in each Open-LIFU firmware/bootloader repository under scripts/.
Ported from Open-Motion's scripts/gen_sbom.py; the differences are that the MCU family
is detected from the tree and that every manifest component is pinned to a hash of its
vendored files, so the manifest cannot silently go stale. Standard library only.

DERIVED from the tree / build (never hand-maintained):
  - root version (git describe, or --version), commit, build configuration
  - STM32 HAL driver, CMSIS Device and CMSIS-Core versions, read from the driver headers
  - newlib (C library linked via --specs=nano.specs) version and the toolchain string,
    passed in from the build image (--newlib-version, --toolchain)
  - build container reference (--build-image)
  - bundled bootloader (firmware production image): repository, tag, SHA-256
  - trusted / signing public-key fingerprints (--trusted-key, --signing-key-fingerprint)

From the reviewed MANIFEST (scripts/sbom-manifest.json): components with no machine-
readable version in the tree (ST middleware, CMOX binary, local snapshots such as jsmn),
with evidence and licences. A component may carry "openwater:files" (comma-separated
globs) and "openwater:files-sha256": the generator hashes those files (CRLF normalised)
and FAILS if the hash differs, so updating vendored code forces a manifest review.
Run with --print-hashes to see the current hashes.

Usage (CI):
  python3 scripts/gen_sbom.py --root . --manifest scripts/sbom-manifest.json \\
      --name open-lifu-console-bl --config Release \\
      --toolchain "$(arm-none-eabi-gcc --version | head -1)" --newlib-version 4.5.0 \\
      --build-image ghcr.io/openwaterhealth/stm32-build-env@sha256:... \\
      --trusted-key research=py-tools/keys/research/ecdsa_public.pem \\
      -o sbom.cdx.json
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid
from pathlib import Path

ORG = {"name": "Openwater (Open Water Internet Inc.)", "url": ["https://www.openwater.health"]}
GH = "https://github.com/OpenwaterHealth/"
TOOL_VERSION = "1.0.0"
FILES_PROP = "openwater:files"
HASH_PROP = "openwater:files-sha256"


# --------------------------------------------------------------------------- helpers
def git(root: Path, *args: str, default: str = "") -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=root, text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return default


def read_define(path: Path, name: str) -> str | None:
    """Numeric value of `#define NAME (0x..U)` / `#define NAME ( 5U)` style macros."""
    if not path.is_file():
        return None
    m = re.search(r"#define\s+%s\s+\(?\s*(0x[0-9A-Fa-f]+|\d+)" % re.escape(name),
                  path.read_text(encoding="utf-8", errors="replace"))
    return m.group(1) if m else None


def triple(path: Path, prefix: str) -> str | None:
    parts = [read_define(path, f"{prefix}_{s}") for s in ("MAIN", "SUB1", "SUB2")]
    return ".".join(str(int(p, 0)) for p in parts) if all(parts) else None


def mcu_family(root: Path) -> str:
    """'F0' from Drivers/STM32F0xx_HAL_Driver, 'L4' from Drivers/STM32L4xx_HAL_Driver, ..."""
    hits = sorted((root / "Drivers").glob("STM32*xx_HAL_Driver"))
    if len(hits) != 1:
        raise SystemExit(f"[gen_sbom] expected exactly one Drivers/STM32*xx_HAL_Driver, found {len(hits)}")
    return hits[0].name[len("STM32"):-len("xx_HAL_Driver")]


def hal_version(root: Path, fam: str) -> str | None:
    # F0 names it __STM32F0xx_HAL_VERSION_*, L4 STM32L4XX_HAL_VERSION_*.
    f = root / f"Drivers/STM32{fam}xx_HAL_Driver/Src/stm32{fam.lower()}xx_hal.c"
    for prefix in (f"__STM32{fam}xx_HAL_VERSION", f"STM32{fam.upper()}XX_HAL_VERSION", f"__STM32{fam.upper()}XX_HAL_VERSION"):
        v = triple(f, prefix)
        if v:
            return v
    return None


def cmsis_device_version(root: Path, fam: str) -> str | None:
    f = root / f"Drivers/CMSIS/Device/ST/STM32{fam}xx/Include/stm32{fam.lower()}xx.h"
    for prefix in (f"__STM32{fam}_DEVICE_VERSION", f"__STM32{fam}xx_CMSIS_VERSION", f"__STM32{fam}_CMSIS_VERSION"):
        v = triple(f, prefix)
        if v:
            return v
    return None


def cmsis_core_version(root: Path) -> str | None:
    f = root / "Drivers/CMSIS/Include/cmsis_version.h"
    main, sub = read_define(f, "__CM_CMSIS_VERSION_MAIN"), read_define(f, "__CM_CMSIS_VERSION_SUB")
    return f"{int(main, 0)}.{int(sub, 0)}.0" if main is not None and sub is not None else None


def files_hash(root: Path, globs: str) -> tuple[str, int]:
    """SHA-256 over (relative path, NUL, content with CRLF->LF) of every matched file, sorted.
    Line endings are normalised so a Windows checkout and the CI checkout agree."""
    paths: set[Path] = set()
    for g in (s.strip() for s in globs.split(",")):
        if g:
            paths.update(p for p in root.glob(g) if p.is_file())
    h = hashlib.sha256()
    for p in sorted(paths, key=lambda q: q.relative_to(root).as_posix()):
        h.update(p.relative_to(root).as_posix().encode() + b"\0")
        h.update(p.read_bytes().replace(b"\r\n", b"\n"))
        h.update(b"\0")
    return h.hexdigest(), len(paths)


def pem_spki_sha256(path: Path) -> str:
    """SHA-256 of the DER SubjectPublicKeyInfo inside a PEM public key."""
    body = re.search(r"-----BEGIN PUBLIC KEY-----(.*?)-----END PUBLIC KEY-----",
                     path.read_text(encoding="ascii"), re.S)
    if not body:
        raise SystemExit(f"[gen_sbom] {path}: not a PEM public key")
    return hashlib.sha256(base64.b64decode("".join(body.group(1).split()))).hexdigest()


def prop_value(c: dict, name: str) -> str | None:
    return next((p["value"] for p in c.get("properties", []) if p.get("name") == name), None)


def comp(name: str, version: str | None, ctype: str = "library", **kw) -> dict:
    c: dict = {"type": ctype, "name": name}
    if version:
        c["version"] = version
    c["bom-ref"] = kw.pop("bom_ref", None) or re.sub(r"[^A-Za-z0-9._-]+", "-", f"{name}@{version or 'unversioned'}").strip("-")
    props = kw.pop("properties", None)
    c.update({k: v for k, v in kw.items() if v is not None})
    if props:
        c["properties"] = [{"name": k, "value": str(v)} for k, v in props.items()]
    return c


def st_supplier() -> dict:
    return {"name": "STMicroelectronics", "url": ["https://www.st.com"]}


# --------------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", type=Path, default=Path("."), help="repository root")
    ap.add_argument("--manifest", type=Path, required=True, help="sbom-manifest.json (reviewed static components)")
    ap.add_argument("--name", help="root component name, e.g. open-lifu-console-bl")
    ap.add_argument("--repo", help="GitHub repository name (default: --name)")
    ap.add_argument("--type", default="firmware", help="root component type")
    ap.add_argument("--version", help="root version (default: git describe --tags --always --dirty)")
    ap.add_argument("--commit", help="commit SHA (default: git rev-parse HEAD)")
    ap.add_argument("--config", default="Release", help="build configuration / preset")
    ap.add_argument("--toolchain", help="arm-none-eabi-gcc --version | head -1, from the build image")
    ap.add_argument("--newlib-version", help="_NEWLIB_VERSION of the build image's toolchain")
    ap.add_argument("--build-image", help="build container reference, ideally with digest")
    ap.add_argument("--bundled-bootloader", nargs=3, metavar=("REPO", "TAG", "SHA256"),
                    help="bootloader bundled into the production image")
    ap.add_argument("--trusted-key", action="append", default=[], metavar="LABEL=PEM",
                    help="public key a bootloader build trusts; recorded as its SPKI SHA-256")
    ap.add_argument("--signing-key-fingerprint", help="SHA-256 (DER SPKI) of the key that signed this image")
    ap.add_argument("--signing-domain", help="research or clinical")
    ap.add_argument("--property", action="append", default=[], metavar="NAME=VALUE", help="extra root property")
    ap.add_argument("--print-hashes", action="store_true", help="print the manifest components' file hashes and exit")
    ap.add_argument("-o", "--output", type=Path)
    a = ap.parse_args()

    root = a.root.resolve()
    manifest = json.loads(a.manifest.read_text(encoding="utf-8"))

    # ---- manifest file pins (also the --print-hashes mode) -------------------------
    stale = []
    for mc in manifest.get("components", []):
        globs = prop_value(mc, FILES_PROP)
        if not globs:
            continue
        digest, n = files_hash(root, globs)
        want = prop_value(mc, HASH_PROP)
        if a.print_hashes:
            print(f"{digest}  {n:4d} files  {mc['name']}")
        elif n == 0:
            stale.append(f"{mc['name']}: no files match {globs!r}")
        elif digest != want:
            stale.append(f"{mc['name']}: {HASH_PROP} is {want}, tree is {digest} ({n} files)")
    if a.print_hashes:
        return 0
    if stale:
        print("[gen_sbom] vendored code differs from scripts/sbom-manifest.json; review the component(s),\n"
              "           update version/evidence, then the hash (--print-hashes):", file=sys.stderr)
        for s in stale:
            print("           - " + s, file=sys.stderr)
        return 2
    if not a.name or not a.output:
        ap.error("--name and -o are required")

    repo = a.repo or a.name
    version = a.version or git(root, "describe", "--tags", "--always", "--dirty", default="unknown")
    commit = a.commit or git(root, "rev-parse", "HEAD", default="unknown")
    ts_epoch = os.environ.get("SOURCE_DATE_EPOCH")
    ts = (dt.datetime.fromtimestamp(int(ts_epoch), dt.timezone.utc) if ts_epoch
          else dt.datetime.now(dt.timezone.utc)).strftime("%Y-%m-%dT%H:%M:%SZ")
    fam = mcu_family(root)

    # ---- root component ----------------------------------------------------------
    root_props = {"openwater:build-config": a.config, "openwater:commit": commit,
                  "openwater:repository": GH + repo}
    if manifest.get("mcu"):
        root_props["openwater:mcu"] = manifest["mcu"]
    if a.build_image:
        root_props["openwater:build-image"] = a.build_image
    for kv in a.trusted_key:
        label, _, pem = kv.partition("=")
        root_props[f"openwater:trusted-public-key-sha256:{label}"] = pem_spki_sha256(root / pem)
    if a.signing_key_fingerprint:
        root_props["openwater:signing-public-key-sha256"] = a.signing_key_fingerprint
    if a.signing_domain:
        root_props["openwater:signing-domain"] = a.signing_domain
    for kv in a.property:
        k, _, v = kv.partition("=")
        root_props[k] = v
    purl = f"pkg:github/OpenwaterHealth/{repo}@{version}"
    rootc = comp(a.name, version, a.type, bom_ref=purl, supplier=ORG, description=manifest.get("description"),
                 licenses=manifest.get("licenses"), purl=purl, properties=root_props)

    components: list[dict] = []

    # ---- derived from the tree / build image --------------------------------------
    fl = fam.lower()
    hal = hal_version(root, fam)
    if not hal or not cmsis_device_version(root, fam):
        # Never emit an SBOM that silently lacks the HAL or the CMSIS device files.
        print(f"[gen_sbom] cannot read the STM32{fam}xx HAL / CMSIS Device version macros; "
              "update hal_version() / cmsis_device_version() for this family", file=sys.stderr)
        return 2
    if hal:
        components.append(comp(f"STM32{fam}xx HAL Driver", hal, supplier=st_supplier(),
                               purl=f"pkg:github/STMicroelectronics/stm32{fl}xx-hal-driver@v{hal}",
                               licenses=[{"license": {"id": "BSD-3-Clause"}}],
                               properties={"openwater:evidence": f"Drivers/STM32{fam}xx_HAL_Driver/Src/stm32{fl}xx_hal.c __STM32{fam}xx_HAL_VERSION_*"}))
    dev = cmsis_device_version(root, fam)
    if dev:
        components.append(comp(f"CMSIS Device STM32{fam}xx", dev, supplier=st_supplier(),
                               purl=f"pkg:github/STMicroelectronics/cmsis-device-{fl}@v{dev}",
                               licenses=[{"license": {"id": "Apache-2.0"}}],
                               properties={"openwater:evidence": f"Drivers/CMSIS/Device/ST/STM32{fam}xx/Include/stm32{fl}xx.h version macros"}))
    core = cmsis_core_version(root)
    if core:
        components.append(comp("CMSIS-Core(M)", core, supplier={"name": "Arm Limited"},
                               purl=f"pkg:github/ARM-software/CMSIS_5@{core}",
                               licenses=[{"license": {"id": "Apache-2.0"}}],
                               properties={"openwater:evidence": "Drivers/CMSIS/Include/cmsis_version.h __CM_CMSIS_VERSION_*"}))
    if a.newlib_version:
        components.append(comp("newlib (newlib-nano C library)", a.newlib_version, supplier={"name": "Red Hat, Inc. and contributors"},
                               cpe=f"cpe:2.3:a:newlib_project:newlib:{a.newlib_version}:*:*:*:*:*:*:*",
                               purl=f"pkg:generic/newlib@{a.newlib_version}",
                               licenses=[{"license": {"name": "BSD-style (newlib COPYING.NEWLIB)"}}],
                               properties={"openwater:evidence": "--specs=nano.specs (cmake/gcc-arm-none-eabi.cmake); _NEWLIB_VERSION of the build image's arm-none-eabi toolchain",
                                           "openwater:linkage": "static; only the referenced objects (string/memory, stdio, malloc support) are linked"}))
    if a.toolchain:
        m = re.search(r"(\d+\.\d+\.Rel\d+|\d+\.\d+\.\d+)", a.toolchain)
        components.append(comp("Arm GNU Toolchain (arm-none-eabi-gcc, incl. libgcc)", m.group(1) if m else None, "application",
                               supplier={"name": "Arm Limited"}, scope="excluded",
                               properties={"openwater:evidence": a.toolchain.strip(),
                                           "openwater:note": "build tool; its libgcc runtime routines are statically linked"}))
    if a.build_image:
        img, _, digest = a.build_image.partition("@")
        components.append(comp(img, digest or "latest", "container", scope="excluded",
                               properties={"openwater:evidence": "docker image used by the build workflow"}))
    if a.bundled_bootloader:
        brepo, btag, bsha = a.bundled_bootloader
        components.append(comp(brepo, btag, "firmware", supplier=ORG,
                               bom_ref=f"pkg:github/OpenwaterHealth/{brepo}@{btag}",
                               purl=f"pkg:github/OpenwaterHealth/{brepo}@{btag}",
                               hashes=[{"alg": "SHA-256", "content": bsha}],
                               properties={"openwater:evidence": f"GitHub release {brepo}@{btag}",
                                           "openwater:consumers": "placed at 0x08000000 in the production image; verifies and launches this firmware"}))

    # ---- manifest (reviewed) -------------------------------------------------------
    derived = {c["name"] for c in components}
    for mc in manifest.get("components", []):
        if mc.get("name") in derived:
            continue  # the tree is the source of truth for these
        c = dict(mc)
        c.setdefault("bom-ref", re.sub(r"[^A-Za-z0-9._-]+", "-", f"{c['name']}@{c.get('version', 'unversioned')}").strip("-"))
        components.append(c)

    # ---- assemble ------------------------------------------------------------------
    bom = {
        "$schema": "http://cyclonedx.org/schema/bom-1.6.schema.json",
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": ts,
            "lifecycles": [{"phase": "build"}],
            "tools": {"components": [comp("gen_sbom.py", TOOL_VERSION, "application", supplier=ORG,
                                          properties={"openwater:evidence": "scripts/gen_sbom.py"})]},
            "authors": [{"name": "Openwater firmware CI"}],
            "supplier": ORG,
            "component": rootc,
            "properties": [{"name": "openwater:generated-by",
                            "value": "scripts/gen_sbom.py; static components from scripts/sbom-manifest.json (hash-pinned)"}],
        },
        "components": components,
        "dependencies": [{"ref": rootc["bom-ref"], "dependsOn": [c["bom-ref"] for c in components if c.get("scope") != "excluded"]}]
                        + [{"ref": c["bom-ref"], "dependsOn": []} for c in components],
    }
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(bom, indent=2) + "\n", encoding="utf-8")
    digest = hashlib.sha256(a.output.read_bytes()).hexdigest()
    print(f"[gen_sbom] {a.output}: {a.name} {version} ({a.config}), {len(components)} components, sha256 {digest[:16]}...")
    for c in components:
        print(f"           - {c['type']:<11} {c['name']} {c.get('version', '')}{'  (excluded)' if c.get('scope') == 'excluded' else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
