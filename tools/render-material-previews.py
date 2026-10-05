#!/usr/bin/env python3
"""Export the editable material SVG assets with librsvg/Cairo. Development only."""
import ctypes as c
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rsvg = c.CDLL('librsvg-2.so.2')
cairo = c.CDLL('libcairo.so.2')
gobject = c.CDLL('libgobject-2.0.so.0')
rsvg.rsvg_handle_new_from_data.argtypes = [c.c_char_p, c.c_size_t, c.c_void_p]
rsvg.rsvg_handle_new_from_data.restype = c.c_void_p
rsvg.rsvg_handle_render_cairo.argtypes = [c.c_void_p, c.c_void_p]
rsvg.rsvg_handle_render_cairo.restype = c.c_int
cairo.cairo_image_surface_create.argtypes = [c.c_int, c.c_int, c.c_int]
cairo.cairo_image_surface_create.restype = c.c_void_p
cairo.cairo_create.argtypes = [c.c_void_p]
cairo.cairo_create.restype = c.c_void_p
cairo.cairo_surface_write_to_png.argtypes = [c.c_void_p, c.c_char_p]
cairo.cairo_surface_write_to_png.restype = c.c_int
cairo.cairo_destroy.argtypes = [c.c_void_p]
cairo.cairo_surface_destroy.argtypes = [c.c_void_p]
gobject.g_object_unref.argtypes = [c.c_void_p]

for source in sorted((root / 'docs/design/prism-concept-b/materials').glob('*.svg')):
    data = source.read_bytes()
    handle = rsvg.rsvg_handle_new_from_data(data, len(data), None)
    if not handle:
        raise RuntimeError(f'Invalid SVG: {source}')
    surface = cairo.cairo_image_surface_create(0, 192, 144)
    context = cairo.cairo_create(surface)
    try:
        if not rsvg.rsvg_handle_render_cairo(handle, context):
            raise RuntimeError(f'Could not render: {source}')
        destination = root / 'demos/demo_settings/assets' / (source.stem + '.png')
        status = cairo.cairo_surface_write_to_png(surface, str(destination).encode())
        if status:
            raise RuntimeError(f'PNG export failed: {destination}, status={status}')
    finally:
        cairo.cairo_destroy(context)
        cairo.cairo_surface_destroy(surface)
        gobject.g_object_unref(handle)
