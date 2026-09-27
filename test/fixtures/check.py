#!/usr/bin/env python3
"""Check fixture resources and confirm the shipped EPUBs match their sources."""
from pathlib import Path
import posixpath
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent


def contents(path):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        assert len(names) == len(set(names)), f'{path.name}: duplicate ZIP entry'
        assert names[0] == 'mimetype', f'{path.name}: mimetype must come first'
        assert archive.getinfo('mimetype').compress_type == zipfile.ZIP_STORED
        assert archive.read('mimetype') == b'application/epub+zip'
        files = {name: archive.read(name) for name in names}
    documents = {name: ET.fromstring(data) for name, data in files.items()
                 if name.endswith(('.xhtml', '.xml', '.opf', '.ncx'))}
    ids = {}
    for name, document in documents.items():
        values = [node.attrib['id'] for node in document.iter() if 'id' in node.attrib]
        assert len(values) == len(set(values)), f'{path.name}/{name}: duplicate element ID'
        ids[name] = set(values)
    for name, document in documents.items():
        for node in document.iter():
            for attribute in ('href', 'src'):
                if attribute not in node.attrib:
                    continue
                link = urlsplit(node.attrib[attribute])
                assert not link.scheme and not link.netloc, f'{path.name}/{name}: external resource'
                target = (posixpath.normpath(posixpath.join(posixpath.dirname(name), unquote(link.path)))
                          if link.path else name)
                assert target in files, f'{path.name}/{name}: missing {target}'
                if link.fragment:
                    assert unquote(link.fragment) in ids.get(target, set()), f'{path.name}/{name}: missing fragment'
    return files


def main():
    scratch = ROOT.parents[1] / 'tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='fixture-check-', dir=scratch) as temporary:
        output = Path(temporary)
        subprocess.run([sys.executable, str(ROOT / 'build.py'), '--output', str(output)],
                       check=True, stdout=subprocess.DEVNULL)
        shipped = {p.name for p in (ROOT / 'books').glob('*.epub')}
        rebuilt = {p.name for p in output.glob('*.epub')}
        assert shipped and shipped == rebuilt, 'Shipped fixtures differ from the generated book list'
        for name in sorted(shipped):
            expected = contents(ROOT / 'books' / name)
            actual = contents(output / name)
            assert expected == actual, f'{name}: rebuild the shipped fixture'
        first = {p.name: p.read_bytes() for p in output.glob('*.epub')}
        subprocess.run([sys.executable, str(ROOT / 'build.py'), '--output', str(output)],
                       check=True, stdout=subprocess.DEVNULL)
        assert all((output / name).read_bytes() == data for name, data in first.items()), 'Nondeterministic build'
    print(f'PASS: {len(shipped)} EPUBs, resources, IDs, source parity and deterministic rebuild')


if __name__ == '__main__':
    main()
