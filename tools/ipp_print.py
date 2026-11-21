#!/usr/bin/env python3
"""Prints a PWG raster test document over IPP, the way Android's default print
service and Windows' IPP class driver do (Print-Job, image/pwg-raster,
sgray_8), then follows the job until it ends. Needs only Python 3 (Pillow
for --image). For the host server and for the board.

  tools/ipp_print.py http://localhost:8631/ipp/print --pages 3
  tools/ipp_print.py http://<board-ip>:631/ipp/print --pages 2 --dpi 600
  tools/ipp_print.py http://<board-ip>:631/ipp/print --image scan.png
  tools/ipp_print.py http://<board-ip>:631/ipp/print --status     (printer state only)
  tools/ipp_print.py URL --pages 5 --cancel-after 8                 (Cancel-Job after 8 s)

Pages: A4 (default) or --letter, at --dpi 300 (default) or 600. Each test
page has a frame 10 mm inside the sheet, its number in large digits, gray
steps and fine lines, so a printed sheet shows position, scale and halftone.
"""
import argparse
import struct
import sys
import time
import urllib.request

# ---------------------------------------------------------------- IPP encoding
TAG_OP, TAG_JOB, TAG_END = 0x01, 0x02, 0x03
INT, BOOL, ENUM, KEYWORD, URI, CHARSET, LANG, MIME, NAME = 0x21, 0x22, 0x23, 0x44, 0x45, 0x47, 0x48, 0x49, 0x42


def attr(tag, name, value):
    if isinstance(value, int) and tag in (INT, ENUM):
        v = struct.pack('>i', value)
    elif tag == BOOL:
        v = bytes([1 if value else 0])
    else:
        v = value.encode()
    return bytes([tag]) + struct.pack('>H', len(name)) + name.encode() + struct.pack('>H', len(v)) + v


def request(op, req_id, uri, extra=b'', job_attrs=b''):
    b = struct.pack('>BBHI', 2, 0, op, req_id)
    b += bytes([TAG_OP]) + attr(CHARSET, 'attributes-charset', 'utf-8') + \
        attr(LANG, 'attributes-natural-language', 'en') + attr(URI, 'printer-uri', uri) + \
        attr(NAME, 'requesting-user-name', 'ipp_print') + extra
    if job_attrs:
        b += bytes([TAG_JOB]) + job_attrs
    return b + bytes([TAG_END])


def parse(resp):
    """status code and {name: [values]} of an IPP response"""
    status = struct.unpack('>H', resp[2:4])[0]
    out, i, name = {}, 8, None
    while i < len(resp):
        tag = resp[i]
        i += 1
        if tag < 0x10:
            if tag == TAG_END:
                break
            continue
        nl = struct.unpack('>H', resp[i:i + 2])[0]
        i += 2
        n = resp[i:i + nl].decode(errors='replace')
        i += nl
        vl = struct.unpack('>H', resp[i:i + 2])[0]
        i += 2
        v = resp[i:i + vl]
        i += vl
        if n:
            name = n
        if tag in (INT, ENUM) and vl == 4:
            v = struct.unpack('>i', v)[0]
        elif tag == BOOL:
            v = bool(v[0])
        else:
            v = v.decode(errors='replace')
        out.setdefault(name, []).append(v)
    return status, out


def post(url, body, timeout=600):
    req = urllib.request.Request(url, data=body, headers={'Content-Type': 'application/ipp'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


# ---------------------------------------------------------------- PWG raster
def pwg_header(w, h, dpi, page_pts):
    hd = bytearray(1796)
    hd[0:7] = b'PwgRaster'[:7]
    struct.pack_into('>II', hd, 276, dpi, dpi)                 # HWResolution
    struct.pack_into('>II', hd, 352, *page_pts)                # PageSize (points)
    struct.pack_into('>IIIIIIII', hd, 372, w, h, 0, 8, 8, w, 0, 18)  # width, height, -, bpc, bpp, bpl, chunky, sgray
    struct.pack_into('>I', hd, 420, 1)                         # NumColors
    return bytes(hd)


def encode_line(px):
    """PWG/URF packbits of one gray line"""
    out, i, n = bytearray(), 0, len(px)
    while i < n:
        j = i + 1
        while j < n and j - i < 128 and px[j] == px[i]:
            j += 1
        if j - i >= 2:
            out += bytes([j - i - 1, px[i]])
            i = j
            continue
        j = i + 1      # literal run until two equal pixels follow
        while j < n and j - i < 128 and not (j + 1 < n and px[j] == px[j + 1]):
            j += 1
        if j - i == 1:      # one pixel: a run of 1 (literals are 2..128)
            out += bytes([0, px[i]])
        else:
            out += bytes([257 - (j - i)]) + bytes(px[i:j])
        i = j
    return bytes(out)


DIGITS = {  # 5x7
    '0': [14, 17, 19, 21, 25, 17, 14], '1': [4, 12, 4, 4, 4, 4, 14], '2': [14, 17, 1, 2, 4, 8, 31],
    '3': [31, 2, 4, 2, 1, 17, 14], '4': [2, 6, 10, 18, 31, 2, 2], '5': [31, 16, 30, 1, 1, 17, 14],
    '6': [6, 8, 16, 30, 17, 17, 14], '7': [31, 1, 2, 4, 8, 8, 8], '8': [14, 17, 17, 14, 17, 17, 14],
    '9': [14, 17, 17, 15, 1, 2, 12]}


def test_page(num, w, h, dpi):
    mm = dpi / 25.4
    e = round(10 * mm)
    rows = []
    label = str(num)
    u = round(6 * mm)       # digit cell: 6 mm per dot
    white = bytes([255]) * w
    for y in range(h):
        if y < e - 1 or y > h - e:
            rows.append(white)
            continue
        row = bytearray(white)
        if y in (e, e + 1, h - e - 1, h - e - 2):
            row[e:w - e] = bytes(w - 2 * e)
        else:
            for x in (e, e + 1, w - e - 2, w - e - 1):
                row[x] = 0
        # the page number, 42 mm tall
        y0 = round(30 * mm)
        if y0 <= y < y0 + 7 * u:
            cy = (y - y0) // u
            for k, ch in enumerate(label):
                bits = DIGITS[ch][cy]
                for cx in range(5):
                    if bits >> (4 - cx) & 1:
                        x0 = round(30 * mm) + k * 6 * u + cx * u
                        row[x0:x0 + u] = bytes(u)
        # gray steps 0..100 %
        g0 = round(90 * mm)
        if g0 <= y < g0 + round(20 * mm):
            sw = round(15 * mm)
            for s in range(11):
                x0 = round(20 * mm) + s * sw
                row[x0:x0 + sw] = bytes([255 - s * 255 // 10]) * sw
        # fine lines
        l0 = round(120 * mm)
        if l0 <= y < l0 + round(30 * mm):
            for k in range(1, 9):
                x0 = round(20 * mm) + k * round(15 * mm)
                row[x0:x0 + k * dpi // 600 + (1 if dpi == 300 and k == 1 else 0)] = bytes(max(1, k * dpi // 600))
        rows.append(bytes(row))
    return rows


def document(args):
    dpi = args.dpi
    w_mm, h_mm, pts = (215.9, 279.4, (612, 792)) if args.letter else (210, 297, (595, 842))
    w, h = round(w_mm / 25.4 * dpi), round(h_mm / 25.4 * dpi)
    out = bytearray(b'RaS2')
    pages = []
    if args.image:
        from PIL import Image
        im = Image.open(args.image).convert('L')
        im.thumbnail((w, h))
        canvas = Image.new('L', (w, h), 255)
        canvas.paste(im, ((w - im.width) // 2, (h - im.height) // 2))
        pages = [[canvas.tobytes()[y * w:(y + 1) * w] for y in range(h)]] * args.pages
    else:
        pages = [test_page(i + 1, w, h, dpi) for i in range(args.pages)]
    for rows in pages:
        out += pwg_header(w, h, dpi, pts)
        for row in rows:
            out += b'\x00' + encode_line(row)
    return bytes(out)


def state_line(attrs):
    st = {3: 'idle', 4: 'processing', 5: 'stopped'}.get((attrs.get('printer-state') or [0])[0], '?')
    reasons = ','.join(attrs.get('printer-state-reasons', []))
    msg = (attrs.get('printer-state-message') or [''])[0]
    return f'printer {st} [{reasons}] {msg}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('url', help='http://host:port/ipp/print')
    ap.add_argument('--pages', type=int, default=1)
    ap.add_argument('--dpi', type=int, default=300, choices=(300, 600))
    ap.add_argument('--letter', action='store_true')
    ap.add_argument('--image', help='print this picture (Pillow) instead of test pages')
    ap.add_argument('--status', action='store_true', help='only show the printer state')
    ap.add_argument('--cancel-after', type=float, help='send Cancel-Job this many seconds after the start')
    args = ap.parse_args()
    uri = args.url.replace('http://', 'ipp://')

    _, a = parse(post(args.url, request(0x000B, 1, uri)))
    print(state_line(a))
    if args.status:
        return 0
    doc = document(args)
    print(f'{args.pages} page(s), {args.dpi} dpi, {len(doc) / 1024:.0f} KB of PWG raster')
    body = request(0x0002, 2, uri, attr(NAME, 'job-name', 'ipp_print test') +
                   attr(MIME, 'document-format', 'image/pwg-raster')) + doc
    t0 = time.time()
    if args.cancel_after is not None:
        import threading

        def cancel():
            time.sleep(args.cancel_after)
            # the job id is not known yet (Print-Job answers at the end): Cancel-My-Jobs
            s, _ = parse(post(args.url, request(0x0039, 9, uri)))
            print(f'  Cancel-My-Jobs sent at {time.time() - t0:.1f} s: status 0x{s:04x}')
        threading.Thread(target=cancel, daemon=True).start()
    status, a = parse(post(args.url, body))
    job = (a.get('job-id') or [None])[0]
    print(f'Print-Job: status 0x{status:04x}, job {job}, {time.time() - t0:.1f} s, '
          f'state {(a.get("job-state") or ["?"])[0]} {",".join(a.get("job-state-reasons", []))}')
    while job is not None:
        s, ja = parse(post(args.url, request(0x0009, 3, uri, attr(INT, 'job-id', job))))
        js = (ja.get('job-state') or [0])[0]
        _, pa = parse(post(args.url, request(0x000B, 4, uri)))
        print(f'  {time.time() - t0:5.1f} s job {job}: state {js} '
              f'{",".join(ja.get("job-state-reasons", []))}, impressions '
              f'{(ja.get("job-impressions-completed") or ["?"])[0]}; {state_line(pa)}')
        if js >= 7:     # canceled, aborted, completed
            break
        time.sleep(2)
    return 0 if status < 0x100 else 1


if __name__ == '__main__':
    sys.exit(main())
