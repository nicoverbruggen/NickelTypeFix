#!/usr/bin/env python3
"""Build the device fixtures offline with Python's standard library."""
import argparse
import html
from pathlib import Path
import re
import struct
import zipfile
import zlib

ROOT = Path(__file__).resolve().parent
BASE_CSS = """
body { font-family: serif; line-height: 1.4; margin: 0; }
h1 { font-size: 1.4em; font-weight: bold; text-align: center; margin: 1.5em 0; }
p { margin: 0; text-indent: 1.1em; orphans: 2; widows: 2; }
p.first { text-indent: 0; }
.byline { font-size: .8em; text-align: center; text-indent: 0; margin: 0 0 2em; }
.ornament { text-align: center; margin: 1.5em 0; }
img { width: 160px; height: 40px; }
"""


def spans(text, paragraph):
    # A trailing sentence space belongs inside its koboSpan. That is the Fix 3 trigger.
    sentences = re.findall(r'.+?(?:[.!?。！？][”’」]?\s+|$)', text)
    return ''.join(f'<span class="koboSpan" id="kobo.{paragraph}.{i}">{html.escape(s)}</span>'
                   for i, s in enumerate(sentences, 1))


def paragraphs(texts, first=True, start=1):
    return ''.join(f'<p class="{"first" if first and i == start else "body"}">{spans(p, i)}</p>'
                   for i, p in enumerate(texts, start))


def chapter(texts, title='Down the Rabbit-Hole'):
    return f'<h1>{html.escape(title)}</h1>' + paragraphs(texts)


def ornament():
    """A small original rule and diamond, with no external image dependency."""
    width, height = 320, 80
    rows = []
    for y in range(height):
        rows.append(b'\0' + bytes(0 if (38 <= y <= 41 and 20 <= x < 300) or
                                 abs(x - 160) + abs(y - 40) < 18 else 255
                                 for x in range(width)))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def book(output, name, chapters, css='', vertical=False):
    title = 'NTF ' + name
    language = 'ja' if vertical else 'en'
    author = '夏目漱石' if vertical else 'Lewis Carroll'
    source = '吾輩は猫である (1905–1906)' if vertical else "Alice’s Adventures in Wonderland (1865)"
    uid = 'urn:nickeltypefix:fixture:' + name
    items = ''.join(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>'
                    for i in range(len(chapters)))
    mode = '<meta name="primary-writing-mode" content="vertical-rl"/>' if vertical else ''
    spine = ' page-progression-direction="rtl"' if vertical else ''
    opf = f'''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">{uid}</dc:identifier><dc:title>{title}</dc:title>
<dc:creator>{author}</dc:creator><dc:language>{language}</dc:language>
<dc:source>{source}</dc:source><dc:description>Public-domain excerpts arranged as a NickelTypeFix test fixture.</dc:description>
<meta property="dcterms:modified">2026-09-27T00:00:00Z</meta>{mode}</metadata>
<manifest>{items}<item id="css" href="style.css" media-type="text/css"/>
<item id="figure" href="ornament.png" media-type="image/png"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"{spine}>{''.join(f'<itemref idref="c{i}"/>' for i in range(len(chapters)))}</spine></package>'''
    toc = ''.join(f'<li><a href="c{i}.xhtml">{html.escape(label)}</a></li>'
                  for i, (label, _) in enumerate(chapters))
    ncx = ''.join(f'<navPoint id="n{i}" playOrder="{i+1}"><navLabel><text>{html.escape(label)}</text></navLabel><content src="c{i}.xhtml"/></navPoint>'
                  for i, (label, _) in enumerate(chapters))
    files = {
        'mimetype': 'application/epub+zip',
        'META-INF/container.xml': '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        'OEBPS/content.opf': opf,
        'OEBPS/style.css': BASE_CSS + css,
        'OEBPS/ornament.png': ornament(),
        'OEBPS/nav.xhtml': f'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{language}"><head><title>Contents</title></head><body><nav epub:type="toc"><h1>Contents</h1><ol>{toc}</ol></nav></body></html>',
        'OEBPS/toc.ncx': f'<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="{uid}"/></head><docTitle><text>{title}</text></docTitle><navMap>{ncx}</navMap></ncx>',
    }
    for i, (label, body) in enumerate(chapters):
        files[f'OEBPS/c{i}.xhtml'] = f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{language}"><head><title>{html.escape(label)}</title><link rel="stylesheet" type="text/css" href="style.css"/></head><body><div id="book-columns"><div id="book-inner">{body}</div></div></body></html>'
    path = output / (name + '.kepub.epub')
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in files.items():
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == 'mimetype' else zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    print(path.name)


def build(output):
    output.mkdir(parents=True, exist_ok=True)
    alice = (ROOT / 'text/alice.txt').read_text().strip().split('\n\n')
    tears = (ROOT / 'text/alice-pool-of-tears.txt').read_text().strip().split('\n\n')
    japanese = (ROOT / 'text/cat.txt').read_text().strip().split('\n\n')
    book(output, '01-baseline', [('Reading sample', chapter(alice[:4])),
         ('Size sweep', ''.join(f'<div style="font-size:{size}%">{paragraphs([alice[0]], start=i)}</div>' for i, size in enumerate(range(85, 116, 5), 1)))])
    book(output, '02-vertical', [('冒頭', chapter(japanese[:4], '吾輩は猫である')),
         ('画はどうかね', chapter(japanese[4:], '吾輩は猫である'))],
         'html,body{-webkit-writing-mode:vertical-rl;writing-mode:vertical-rl}p{line-height:1.6}h1{font-size:1.3em;font-weight:normal;margin:0 0 1em}', True)
    book(output, '03-justification', [('Sentence boundaries', chapter(tears, 'The Pool of Tears'))],
         'p{text-align:justify;hyphens:none;-webkit-hyphens:none}')
    book(output, '04-punctuation', [('Dialogue', chapter(alice[6:]))],
         'p{text-align:justify;hyphens:none;-webkit-hyphens:none}')
    book(output, '05-tracking', [('Tracked title', '<h1 class="tracked">Down the Rabbit-Hole</h1><p class="byline tracked">LEWIS CARROLL</p>'+paragraphs(alice[:4]))],
         '.tracked{letter-spacing:.2em}')
    book(output, '06-fonts', [(f'Chapter {i+1}', chapter(alice[i:i+3])) for i in range(3)])
    book(output, '07-capitals', [('Capitals in prose', chapter(alice[:4])),
         ('Repeated opening', chapter([alice[0]]*4))])
    book(output, '09-page-edges', [('Page boundaries', chapter(alice*12)),
         ('Next chapter', chapter(alice[:3]))], 'body,p{line-height:1.0}p{orphans:1;widows:1}')
    image = '<div class="ornament"><span class="koboSpan" id="kobo.100.1"><img src="ornament.png" alt="Ornament"/></span></div>'
    opener = '<h1>Down the Rabbit-Hole</h1>'+image+'<p class="first"><span class="initial">A</span>'+spans(alice[0][1:],1)+'</p>'+paragraphs(alice[1:4],False,2)
    controls = '<h1>Down the Rabbit-Hole</h1><div class="left-image"><img src="ornament.png" alt="Left-aligned ornament"/></div><p class="first"><span class="floating">A</span>'+spans(alice[0][1:],1)+'</p>'+paragraphs(alice[1:4],False,2)
    book(output, '10-openers', [('Inline initial and centred image', opener), ('Floated initial and left image', controls)],
         '.initial{font-size:2.6em}.floating{font-size:2.6em;float:left;margin-right:.08em}.left-image{text-align:left}')
    sc = re.sub(r'\b(Alice|White Rabbit)\b', r'<span class="smallcaps">\1</span>', paragraphs(alice[:4]))
    book(output, '14-small-caps', [('Small caps in prose', '<h1>Down the Rabbit-Hole</h1>'+sc),
         ('Ordinary capitals', chapter(alice[:4]))], '.smallcaps{font-variant:small-caps}')
    book(output, '15-long-chapter', [('Long chapter', chapter(alice*500)), ('Short chapter', chapter(alice[:3]))])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'books')
    build(parser.parse_args().output)
