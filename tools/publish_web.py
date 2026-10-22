"""Put a firmware build into the web installer: copies the .bin under
web/firmware/<board>/ and records it as the newest release in
web/firmware/manifest.json.

usage: publish_web.py BOARD FIRMWARE.bin VERSION [NOTES]
"""
import hashlib
import json
import os
import shutil
import sys

board, binary, version = sys.argv[1:4]
notes = sys.argv[4] if len(sys.argv) > 4 else ''
root = os.path.join(os.path.dirname(__file__), '..', 'web')
manifest_path = os.path.join(root, 'firmware', 'manifest.json')
name = f'my-kontrol-{board}-{version}.bin'
rel = f'firmware/{board}/{name}'
os.makedirs(os.path.join(root, 'firmware', board), exist_ok=True)
shutil.copyfile(binary, os.path.join(root, rel))
data = open(binary, 'rb').read()

manifest = json.load(open(manifest_path))
entry = next(b for b in manifest['boards'] if b['id'] == board)
releases = [r for r in entry['releases'] if r['version'] != version and r.get('sha256') != 'PLACEHOLDER']
releases.insert(0, {'version': version, 'file': rel, 'sha256': hashlib.sha256(data).hexdigest(),
                    'size': len(data), 'notes': notes})
entry['releases'] = releases
manifest.pop('_comment', None)
json.dump(manifest, open(manifest_path, 'w'), indent=2)
print(f'{rel}: {len(data)} bytes')
