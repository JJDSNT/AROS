#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
"""Embed a binary file as a C byte array: bin_to_header.py <in> <out.h> <symbol>"""
import sys

src, dst, sym = sys.argv[1:4]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write(f'/* Generated from {src.rsplit("/", 1)[-1]} by "make fixtures"; do not edit. */\n')
    f.write('#pragma once\n\n')
    f.write(f'static const unsigned char {sym}[] = {{\n')
    for i in range(0, len(data), 12):
        f.write('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 12]) + ',\n')
    f.write('};\n')
