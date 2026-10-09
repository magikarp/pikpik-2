#!/usr/bin/env python3
"""Patch the pinned Dawn source for iOS (run by cmake/renderer.cmake on iOS).

Dawn enables TextureCompressionBC on iOS when the deployment target is 16.4+
(PhysicalDeviceMTL.mm), but its BC pixel-format tables are macOS-only, so
creating the first BC texture aborts in MetalPixelFormat. Widen those guards to
the same condition the feature uses. Idempotent.
"""
import sys
from pathlib import Path

root = Path(sys.argv[1]) / 'src/dawn/native/metal'
condition = ('#if DAWN_PLATFORM_IS(MACOS) || \\\n'
             '    (defined(__IPHONE_16_4) && __IPHONE_OS_VERSION_MIN_REQUIRED >= __IPHONE_16_4)\n')
for name, follows in (('UtilsMetal.mm', '        case wgpu::TextureFormat::BC1RGBAUnorm:'),
                      ('TextureMTL.mm', '            SRGB_PAIR(MTLPixelFormatBC1_RGBA,')):
    path = root / name
    text = path.read_text()
    if condition + follows in text:
        continue
    old = '#if DAWN_PLATFORM_IS(MACOS)\n' + follows
    if text.count(old) != 1:
        raise SystemExit(f'patch_dawn: drift in {name}')
    path.write_text(text.replace(old, condition + follows))
    print(f'patch_dawn: BC formats enabled for iOS 16.4+ in {name}')
