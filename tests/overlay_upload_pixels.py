#!/usr/bin/python3
# Manual pixel check using the Cinnamon libraries and the benchmark's environment.
# ctypes only reads back pixels through the installed Cogl library: GI does not
# expose that output buffer. No native helper is compiled or loaded into a shell.
import ctypes
import os
import tempfile

import gi

gi.require_version('Cogl', '0')
gi.require_version('Clutter', '0')
from gi.repository import Clutter, Cogl, GLib

width = height = 1024
stride = width * 4
patch_x, patch_y, patch_width, patch_height = 137, 211, 512, 512
source_x, source_y = 17, 23
pixel_format = Cogl.PixelFormat.BGRA_8888_PRE
background = bytes(v for y in range(height) for x in range(width)
                   for v in (x % 256, y % 256, (x + y) % 256, 255))
target_image = Clutter.Image.new()
assert target_image.set_bytes(GLib.Bytes.new(background), pixel_format, width, height, stride)
whole_image = Clutter.Image.new()
target_texture = target_image.get_texture()
target = Cogl.Offscreen.new_with_texture(target_texture)
assert target.allocate()
lib = ctypes.CDLL('libmuffin-cogl-0.so.0')
lib.cogl_texture_get_data.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p]
lib.cogl_texture_get_data.restype = ctypes.c_int
buffer = (ctypes.c_ubyte * len(background))()
expected = bytearray(background)
with tempfile.TemporaryFile() as source_file:
    source_file.truncate(stride * height)
    mapped = GLib.MappedFile.new_from_fd(source_file.fileno(), False)
    shared = mapped.get_bytes()
    offset = source_y * stride + source_x * 4
    length = (patch_height - 1) * stride + patch_width * 4
    for frame in range(3):
        # Transparent pixels also have zero RGB, preserving premultiplication.
        data = bytearray(v for y in range(height) for x in range(width)
                         for v in (((x + frame * 7) % 64, (y + frame * 13) % 64,
                                    (x + y + frame * 17) % 64, 128)
                                   if (x + y + frame) % 17 else (0, 0, 0, 0)))
        assert os.pwrite(source_file.fileno(), data, 0) == len(data)
        assert whole_image.set_bytes(shared, pixel_format, width, height, stride)
        whole_texture = whole_image.get_texture()
        assert lib.cogl_texture_get_data(hash(whole_texture), int(pixel_format), stride,
                                         buffer) == len(data)
        assert bytes(buffer) == data, f'full upload frame {frame} differs from mmap'
        area_bytes = GLib.Bytes.new_from_bytes(shared, offset, length)
        staging_image = Clutter.Image.new()
        assert staging_image.set_bytes(area_bytes, pixel_format, patch_width, patch_height, stride)
        staging = Cogl.Offscreen.new_with_texture(staging_image.get_texture())
        assert staging.allocate()
        assert Cogl.blit_framebuffer(staging, target, 0, 0, patch_x, patch_y,
                                     patch_width, patch_height)
        target.finish()
        for row in range(patch_height):
            src = offset + row * stride
            dst = (patch_y + row) * stride + patch_x * 4
            expected[dst:dst + patch_width * 4] = data[src:src + patch_width * 4]
        assert lib.cogl_texture_get_data(hash(target_texture), int(pixel_format), stride,
                                         buffer) == len(background)
        assert bytes(buffer) == expected, f'partial upload frame {frame} differs from reference'
        print(f'frame {frame}: all {width * height} pixels match; BGRA, alpha, '
              'orientation, stride, reused full-upload content, changed mmap data, '
              'untouched pixels verified')
