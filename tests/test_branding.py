#!/usr/bin/env python3
"""Validate approved branding assets and, on Windows, a built executable.

Examples:
    python tests/test_branding.py --assets assets/branding
    python tests/test_branding.py --assets assets/branding --exe path/to/AlienStudio.exe

Only the Python standard library is required. The executable check reads Windows
resources without executing the application; it does not test shell icon caches.
"""

import argparse
import ctypes
import hashlib
from pathlib import Path
import struct
import sys
import unittest
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
APPROVED_ICO_SHA256 = "94ae61b3225fdf887128301952c1fa4ae7f6820825c711700efadf237af9102e"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def png_dimensions(data):
    """Check PNG chunk bounds, checksums, header and compressed image data."""
    require(data.startswith(PNG_SIGNATURE), "Invalid PNG signature")
    offset = len(PNG_SIGNATURE)
    chunks = []
    compressed = bytearray()
    header = None
    while offset < len(data):
        require(offset + 12 <= len(data), "Truncated PNG chunk")
        length, kind = struct.unpack_from(">I4s", data, offset)
        end = offset + 12 + length
        require(end <= len(data), "PNG chunk extends beyond file")
        payload = data[offset + 8 : end - 4]
        checksum = struct.unpack_from(">I", data, end - 4)[0]
        require(zlib.crc32(kind + payload) == checksum, "Invalid PNG chunk checksum")
        if not chunks:
            require(kind == b"IHDR" and length == 13, "PNG must start with IHDR")
            header = struct.unpack(">IIBBBBB", payload)
        else:
            require(kind != b"IHDR", "Duplicate PNG IHDR")
        chunks.append(kind)
        if kind == b"IDAT":
            compressed.extend(payload)
        offset = end
        if kind == b"IEND":
            require(length == 0 and offset == len(data), "Invalid PNG ending")
            break

    require(chunks and chunks[-1] == b"IEND", "Missing PNG IEND")
    require(compressed, "Missing PNG image data")
    width, height, depth, colour, compression, filtering, interlace = header
    require(width > 0 and height > 0, "Invalid PNG dimensions")
    depths = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8),
              4: (8, 16), 6: (8, 16)}
    require(colour in depths and depth in depths[colour], "Invalid PNG pixel format")
    require(compression == 0 and filtering == 0 and interlace in (0, 1),
            "Unsupported PNG encoding")
    decoder = zlib.decompressobj()
    decoded = decoder.decompress(compressed) + decoder.flush()
    require(decoder.eof and not decoder.unused_data, "Invalid PNG compressed stream")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    passes = ((0, 0, 1, 1),) if interlace == 0 else (
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
        (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))
    cursor = 0
    for x, y, step_x, step_y in passes:
        pass_width = max(0, (width - x + step_x - 1) // step_x)
        pass_height = max(0, (height - y + step_y - 1) // step_y)
        if not pass_width or not pass_height:
            continue
        stride = (pass_width * channels * depth + 7) // 8 + 1
        require(cursor + stride * pass_height <= len(decoded), "Truncated PNG scanlines")
        require(all(decoded[cursor + row * stride] <= 4 for row in range(pass_height)),
                "Invalid PNG scanline filter")
        cursor += stride * pass_height
    require(cursor == len(decoded), "Unexpected PNG scanline data")
    return width, height


def icon_frames(data):
    """Return ICO entries and payloads after checking the complete directory."""
    require(len(data) >= 6, "Truncated ICO header")
    reserved, kind, count = struct.unpack_from("<HHH", data)
    require(reserved == 0 and kind == 1, "Invalid ICO header")
    require(count == len(ICON_SIZES), "ICO must contain all seven approved sizes")
    directory_end = 6 + 16 * count
    require(directory_end <= len(data), "Truncated ICO directory")
    frames = []
    intervals = []
    for index, size in enumerate(ICON_SIZES):
        entry = struct.unpack_from("<BBBBHHII", data, 6 + 16 * index)
        width, height, colours, entry_reserved, planes, depth, length, offset = entry
        require((width or 256, height or 256) == (size, size),
                "ICO sizes must be unique and ordered: " + str(ICON_SIZES))
        require(colours == 0 and entry_reserved == 0 and planes in (0, 1) and depth == 32,
                "Invalid ICO frame metadata")
        require(length > 0 and offset >= directory_end and offset + length <= len(data),
                "ICO payload extends outside image data")
        intervals.append((offset, offset + length))
        payload = data[offset : offset + length]
        require(png_dimensions(payload) == (size, size), "ICO image dimensions differ")
        frames.append((entry, payload))
    intervals.sort()
    require(all(left[1] <= right[0] for left, right in zip(intervals, intervals[1:])),
            "Overlapping ICO payloads")
    return frames


class WindowsResources:
    """Read resources using the same Windows resource loader as the application."""

    def __init__(self, path):
        from ctypes import wintypes

        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.callback_type = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t)
        self.api.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
        self.api.LoadLibraryExW.restype = wintypes.HMODULE
        self.api.FreeLibrary.argtypes = [wintypes.HMODULE]
        self.api.FreeLibrary.restype = wintypes.BOOL
        self.api.EnumResourceNamesW.argtypes = [
            wintypes.HMODULE, ctypes.c_void_p, self.callback_type, ctypes.c_ssize_t]
        self.api.EnumResourceNamesW.restype = wintypes.BOOL
        self.api.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
        self.api.FindResourceW.restype = wintypes.HRSRC
        self.api.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
        self.api.SizeofResource.restype = wintypes.DWORD
        self.api.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
        self.api.LoadResource.restype = wintypes.HGLOBAL
        self.api.LockResource.argtypes = [wintypes.HGLOBAL]
        self.api.LockResource.restype = ctypes.c_void_p
        # LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE: no code runs.
        self.module = self.api.LoadLibraryExW(str(path.resolve()), None, 0x02 | 0x20)
        if not self.module:
            raise ctypes.WinError(ctypes.get_last_error())

    def names(self, kind):
        names = []

        @self.callback_type
        def collect(module, resource_type, name, parameter):
            names.append(name if name <= 0xFFFF else ctypes.wstring_at(name))
            return True

        if not self.api.EnumResourceNamesW(self.module, kind, collect, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        return names

    def read(self, kind, name):
        reference = ctypes.c_void_p(name) if isinstance(name, int) else ctypes.c_wchar_p(name)
        resource = self.api.FindResourceW(self.module, ctypes.cast(reference, ctypes.c_void_p), kind)
        if not resource:
            raise ctypes.WinError(ctypes.get_last_error())
        size = self.api.SizeofResource(self.module, resource)
        loaded = self.api.LoadResource(self.module, resource)
        pointer = self.api.LockResource(loaded) if loaded else None
        if not size or not pointer:
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.string_at(pointer, size)

    def close(self):
        self.api.FreeLibrary(self.module)


class BrandingTests(unittest.TestCase):
    assets = None
    executable = None
    ico_only = False

    def test_corrected_ico_is_approved_original(self):
        data = (self.assets / "AlienStudio_Icon_Corrected.ico").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), APPROVED_ICO_SHA256,
                         "Use the supplied corrected ICO unchanged")
        icon_frames(data)

    def test_high_resolution_png(self):
        if self.ico_only:
            self.skipTest("PNG NOT CHECKED: --ico-only requested for interim validation")
        data = (self.assets / "AlienStudio_Icon_Corrected.png").read_bytes()
        width, height = png_dimensions(data)
        self.assertEqual(width, height, "Keep the supplied square artwork without stretching")
        self.assertGreaterEqual(width, 1024, "Keep the original high-resolution PNG")

    def test_executable_contains_approved_icon_frames(self):
        if self.executable is None:
            self.skipTest("Pass --exe on Windows to check compiled icon resources")
        expected = icon_frames((self.assets / "AlienStudio_Icon_Corrected.ico").read_bytes())
        resources = WindowsResources(self.executable)
        try:
            groups = resources.names(14)  # RT_GROUP_ICON
            self.assertTrue({1, 2}.issubset(groups),
                            "JUCE window icons need groups 1 and 2 for both icon ordinals")
            for name in groups:
                with self.subTest(group=name):
                    group = resources.read(14, name)
                    self.assertGreaterEqual(len(group), 6)
                    self.assertEqual(struct.unpack_from("<HHH", group), (0, 1, len(ICON_SIZES)))
                    self.assertEqual(len(group), 6 + 14 * len(ICON_SIZES))
                    identifiers = set()
                    for index, (entry, payload) in enumerate(expected):
                        compiled = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * index)
                        self.assertEqual(compiled[:7], entry[:7])
                        self.assertNotIn(compiled[7], identifiers, "Duplicate RT_ICON identifier")
                        identifiers.add(compiled[7])
                        self.assertEqual(resources.read(3, compiled[7]), payload,
                                         "Compiled icon must preserve the supplied frame bytes")
        finally:
            resources.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", required=True, type=Path, help="Directory containing corrected PNG and ICO")
    parser.add_argument("--exe", type=Path, help="Built Windows executable (requires Windows)")
    parser.add_argument("--ico-only", action="store_true",
                        help="Interim validation only: explicitly skip the high-resolution PNG check")
    arguments = parser.parse_args()
    if arguments.exe is not None and sys.platform != "win32":
        parser.error("--exe uses the Windows resource loader and requires Windows")
    BrandingTests.assets = arguments.assets
    BrandingTests.executable = arguments.exe
    BrandingTests.ico_only = arguments.ico_only
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(BrandingTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
