"""Build a runtime-only Windows ZIP from an explicit file allowlist."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

RUNTIME_FILES = (
    "kyty_emulator.exe", "launcher.exe", "libwinpthread-1.dll", "icuuc.dll",
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Network.dll", "Qt6Svg.dll", "Qt6Widgets.dll",
    "generic/qtuiotouchplugin.dll", "iconengines/qsvgicon.dll",
    "imageformats/qgif.dll", "imageformats/qico.dll", "imageformats/qjpeg.dll", "imageformats/qsvg.dll",
    "networkinformation/qnetworklistmanager.dll", "platforms/qwindows.dll",
    "styles/qmodernwindowsstyle.dll", "tls/qcertonlybackend.dll", "tls/qschannelbackend.dll",
)

LICENSE_FILES = {
    "cpuinfo": "3rdparty/cpuinfo/LICENSE", "fmt": "3rdparty/fmt/LICENSE",
    "imgui": "3rdparty/imgui/LICENSE.txt", "LibAtrac9": "3rdparty/LibAtrac9/LICENSE",
    "magic_enum": "3rdparty/magic_enum/LICENSE", "nlohmann_json": "3rdparty/nlohmann_json/LICENSE.MIT",
    "SDL3": "3rdparty/SDL3/LICENSE.txt", "spdlog": "3rdparty/spdlog/LICENSE",
    "SPIRV-Headers": "3rdparty/SPIRV-Headers/LICENSE", "SPIRV-Tools": "3rdparty/SPIRV-Tools/LICENSE",
    "stb": "3rdparty/stb/LICENSE", "tracy": "3rdparty/tracy/LICENSE",
    "Vulkan-Headers": "3rdparty/Vulkan-Headers/LICENSE.md",
    "VulkanMemoryAllocator": "3rdparty/VulkanMemoryAllocator/LICENSE.txt",
    "xxHash": "3rdparty/xxHash/LICENSE", "winpthread": "3rdparty/winpthread/COPYING",
    "opus": "_Build/rt/_deps/opus-src/COPYING", "ZArchive": "_Build/rt/_deps/zarchive_source-src/LICENSE",
    "zstd": "_Build/rt/_deps/zstd-src/LICENSE", "Zydis": "_Build/rt/_deps/zydis-src/LICENSE",
    "Zycore": "_Build/rt/_deps/zydis-src/dependencies/zycore/LICENSE",
    "Xbyak": "_Build/rt/_deps/xbyak-src/COPYRIGHT",
}

DEFAULT_SETTINGS = """# KytyPS5 release defaults; contains no game paths or user profile.
auto-optimize = false
gpu-timestamp-scale = 100
ray-tracing = false
master-volume = 100
audio-mute = false
aniso = 16
res-scale = 100
motion-blur = true
depth-of-field = true
bloom = true
ambient-occlusion = true
async-submit = true
pipeline-libraries = true
async-pipelines = true
relaxed-readback = true
speculative-draws = true
record-thread = true
hardware-buffer-bounds = true
osd-mode = 2
osd-alignment = 1
printf-direction = File
printf-output-file = _kyty.txt
"""

README = """KytyPS5 Windows x64 prerelease

Extract the whole ZIP into a new folder and start launcher.exe. Keep the DLLs
and plugin folders together. Select your own extracted game directory.
Your game files, saves, settings profiles and pipeline caches are not included.

Ray tracing and automatic optimization default to off. RT uses guest BVH
intersections through Vulkan compute; enable it in Graphics settings or F2,
then fully restart the game. F2 displays the active and next-launch states.
Touchpad input belongs to the game; it no longer opens quality settings.

File logging is automatic. Each default log has a unique UTC-dated _kyty name.
Diagnostic filtering removes host paths, known host identities and sensitive
fields. Do not put personal information into freeform tester descriptions.
Signed Qt dependencies retain their standard vendor build/debug paths; these
are vendor data, not tester or developer installation directories.

This release includes APR address-wait ordering, RT setting persistence,
controller speaker fallback gain, input trace gating and semaphore diagnostics.
See RELEASE_NOTES.md for the complete validation results and known limitations.
No sustained 60 FPS, complete RT visual fidelity or all-game stability is claimed.

BUILD-MANIFEST.json records exact binary hashes and source/package revisions.
Licenses for distributed components are under licenses/. Qt DLLs are dynamically
linked and may be replaced with compatible versions. Project source is available
at https://github.com/Xx-LiDAF-xX/KytyPS5 at the recorded source revision.
"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    parser.add_argument("--source-revision", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = Path(args.output).resolve()
    if output.exists():
        raise ValueError("Output already exists; choose a new ZIP filename")
    source_revision = subprocess.check_output(
        ["git", "rev-parse", args.source_revision + "^{commit}"], cwd=root, text=True).strip()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    files = {name: (root / "bin" / name).read_bytes() for name in RUNTIME_FILES}
    files["kyty_settings.ini"] = DEFAULT_SETTINGS.encode()
    files["Kyty.ini"] = b"; Fresh portable launcher profile. No game paths or user data.\n"
    files["README.txt"] = README.encode()
    files["LICENSE.txt"] = (root / "LICENSE").read_bytes()
    files["RELEASE_NOTES.md"] = (root / "RELEASE_NOTES.md").read_bytes()
    for name, relative in LICENSE_FILES.items():
        files["licenses/" + name + ".txt"] = (root / relative).read_bytes()
    for name in ("LGPL-3.0-only.txt", "GPL-3.0-only.txt", "Qt-GPL-exception-1.0.txt", "FFmpeg-LGPL-2.1.txt"):
        files["licenses/" + name] = (root / "_Build/rt/release-licenses" / name).read_bytes()
    manifest = {
        "SourceRevision": source_revision, "PackageRevision": revision,
        "Platform": "Windows x64", "ReleaseType": "prerelease",
        "Files": {name: {"Bytes": len(data), "SHA256": hashlib.sha256(data).hexdigest()}
                  for name, data in sorted(files.items())},
    }
    files["BUILD-MANIFEST.json"] = (json.dumps(manifest, indent=2) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(files):
            raise ValueError("ZIP content verification failed")
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(checksum + "  " + output.name + "\n")
    print(f"Packaged {len(files)} necessary runtime/documentation/license files: {output.name}")

if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Do not dump exception messages or tracebacks containing local paths.
        print("Release packaging failed:", type(error).__name__)
        raise SystemExit(1)
