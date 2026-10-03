"""Losslessly decode the engine's bounded uncompressed 24-bit TGA to PNG."""
import struct
import zlib

def convert(data):
    if len(data) < 18 or data[1] or data[2] != 2 or data[16] != 24 or data[17] & 0x10:
        raise ValueError('Expected native uncompressed 24-bit TGA')
    width, height = struct.unpack_from('<HH', data, 12)
    if not 1 <= width <= 4096 or not 1 <= height <= 2160 or len(data) != 18 + data[0] + width * height * 3:
        raise ValueError('Native screenshot dimensions/bytes exceed bound')
    rgb = bytearray(data[18 + data[0]:]); blue = rgb[0::3]; rgb[0::3] = rgb[2::3]; rgb[2::3] = blue
    rows = range(height) if data[17] & 0x20 else range(height - 1, -1, -1)
    raw = b''.join(b'\0' + rgb[row * width * 3:(row + 1) * width * 3] for row in rows)
    def chunk(name, value):
        return struct.pack('>I', len(value)) + name + value + struct.pack('>I', zlib.crc32(name + value))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 3)) + chunk(b'IEND', b''))
