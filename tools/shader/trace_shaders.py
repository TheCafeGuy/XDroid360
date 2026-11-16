"""Extract unique Xenos shader ucode (IM_LOAD / IM_LOAD_IMMEDIATE) from a xenia .xtr trace.
Usage: trace_shaders.py <trace.xtr> <out_dir>
Writes <out_dir>/{vs,ps}_<sha1-12>.bin (big-endian dwords as in guest memory)."""
import hashlib, os, struct, sys


def snappy_decode(src):
    # Raw snappy block format.
    pos, n, shift = 0, 0, 0
    while True:
        b = src[pos]; pos += 1
        n |= (b & 0x7F) << shift; shift += 7
        if b < 0x80:
            break
    out = bytearray()
    while pos < len(src):
        tag = src[pos]; pos += 1
        t = tag & 3
        if t == 0:
            ln = tag >> 2
            if ln >= 60:
                nb = ln - 59
                ln = int.from_bytes(src[pos:pos + nb], 'little'); pos += nb
            ln += 1
            out += src[pos:pos + ln]; pos += ln
            continue
        if t == 1:
            ln = ((tag >> 2) & 7) + 4
            off = ((tag >> 5) << 8) | src[pos]; pos += 1
        elif t == 2:
            ln = (tag >> 2) + 1
            off = int.from_bytes(src[pos:pos + 2], 'little'); pos += 2
        else:
            ln = (tag >> 2) + 1
            off = int.from_bytes(src[pos:pos + 4], 'little'); pos += 4
        start = len(out) - off
        for i in range(ln):
            out.append(out[start + i])
    assert len(out) == n, (len(out), n)
    return bytes(out)


def main(path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    data = open(path, 'rb').read()
    pos = 4 + 40 + 4
    pending = None  # shader type waiting for the IM_LOAD memory read
    seen = {}
    loads = {'vs': 0, 'ps': 0}
    u32 = lambda p: struct.unpack_from('<I', data, p)[0]
    while pos + 4 <= len(data):
        t = u32(pos)
        if t in (0, 2, 4):  # buffer/packet start
            base, count = u32(pos + 4), u32(pos + 8)
            body = data[pos + 12:pos + 12 + count * 4]
            pos += 12 + (count * 4 if t == 4 else 0)
            if t == 4 and count >= 3:
                hdr = struct.unpack_from('>I', body, 0)[0]
                if hdr >> 30 == 3:
                    op = (hdr >> 8) & 0x7F
                    d0, d1 = struct.unpack_from('>II', body, 4)
                    if op == 0x27:
                        pending = 'vs' if (d0 & 3) == 0 else 'ps'
                    elif op == 0x2B:
                        kind = 'vs' if d0 == 0 else 'ps'
                        size = d1 & 0xFFFF
                        save(out_dir, kind, body[12:12 + size * 4], seen, loads)
        elif t in (1, 3, 5):
            pos += 4
        elif t in (6, 7):
            enc, elen, dlen = u32(pos + 8), u32(pos + 12), u32(pos + 16)
            raw = data[pos + 20:pos + 20 + elen]
            pos += 20 + elen
            if t == 6 and pending:
                buf = snappy_decode(raw) if enc == 1 else raw
                save(out_dir, pending, buf, seen, loads)
                pending = None
        elif t == 8:
            pos += 12 + u32(pos + 8)
        elif t == 9:
            pos += 8
        elif t == 10:
            pos += 24 + u32(pos + 20)
        elif t == 11:
            pos += 16 + u32(pos + 12)
        else:
            sys.exit(f'unknown command {t} at {pos}')
    print(f"loads vs={loads['vs']} ps={loads['ps']} unique={len(seen)} "
          f"(vs={sum(1 for k in seen if k.startswith('vs'))})")


def save(out_dir, kind, buf, seen, loads):
    loads[kind] += 1
    h = hashlib.sha1(buf).hexdigest()[:12]
    key = f'{kind}_{h}'
    if key not in seen:
        seen[key] = 1
        open(os.path.join(out_dir, key + '.bin'), 'wb').write(buf)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
