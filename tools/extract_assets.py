#!/usr/bin/env python3
"""Decode selected local RL packages into private working copies, then export with UModel."""
import argparse
import importlib
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def decode(source, target, editor):
    raw = source.read_bytes()
    with source.open('rb') as stream:
        summary = editor.parse_file_summary(stream)
        meta = editor.parse_file_compression_metadata(stream)
        provider = editor.DecryptionProvider(str(ROOT/'.deps/RLUPKTools/keys.txt'))
        header = editor.decrypt_data(stream, summary, meta, provider)
    at = meta.compressed_chunks_offset
    count = struct.unpack_from('<I', header, at)[0]
    if not 1 <= count <= 100000:raise ValueError('Invalid chunk count')
    candidates = []
    # RL 2.76's licensee-34 table adds twelve bytes to each chunk entry.
    # Select by validating every source chunk tag, range and contiguous output,
    # rather than treating the new fields as compression offsets.
    for stride in (24, 36):
        if at+4+(count-1)*stride+24 > len(header):continue
        chunks = [editor.FCompressedChunk(*struct.unpack_from('<qiqi', header, at+4+i*stride)) for i in range(count)]
        end = summary.depends_offset
        for c in chunks:
            if (c.uncompressed_offset != end or not 0 < c.uncompressed_size < 512*1024*1024 or
                not 0 < c.compressed_offset <= len(raw)-16 or not 0 < c.compressed_size <= len(raw)-c.compressed_offset or
                raw[c.compressed_offset:c.compressed_offset+4] != b'\xc1\x83\x2a\x9e'):
                break
            end += c.uncompressed_size
        else:candidates.append(chunks)
    if len(candidates) != 1:raise ValueError('Unknown compression table layout; preserving original package')
    summary.compressed_chunks = candidates[0]
    target.parent.mkdir(parents=True, exist_ok=True)
    with source.open('rb') as stream, target.open('wb+') as output:
        output.write(raw[:summary.name_offset]);output.write(header)
        editor.process_compressed_data(output, stream, summary)
    editor.parse_decrypted_package(target)  # validate tables before handing it to the exporter

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('package', help='Installed package basename, for example Body_Norton_SF.upk')
    p.add_argument('object', nargs='?', help='Specific mesh or texture export name')
    p.add_argument('--rl-root', type=Path, default=Path.home()/'.local/share/Steam/steamapps/common/rocketleague')
    p.add_argument('--list', action='store_true')
    a = p.parse_args()
    if Path(a.package).name != a.package:p.error('Pass a package basename')
    sys.path.insert(0, str(ROOT/'.deps/RLUPKTools'))
    editor = importlib.import_module('rl_upk_editor')
    cooked = a.rl_root/'TAGame/CookedPCConsole'
    matches = [f for f in cooked.iterdir() if f.name.lower() == a.package.lower()]
    if len(matches) != 1:p.error('Installed package not found or ambiguous')
    source = matches[0]
    target = ROOT/'build/assets/decrypted'/source.name
    decode(source, target, editor)
    for texture_cache in cooked.glob('*.tfc'):
        link = target.parent/texture_cache.name
        if not link.exists():link.symlink_to(texture_cache.resolve())
    command = [str(ROOT/'.deps/UEViewer/umodel'), '-game=rocketleague', '-noanim', '-dds',
               '-out='+str(ROOT/'build/assets/rl'), '-list' if a.list else '-export', str(target)]
    if a.object:command.append(a.object)
    subprocess.run(command, cwd=ROOT, check=True)

if __name__ == '__main__':main()
