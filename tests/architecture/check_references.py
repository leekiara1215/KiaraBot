"""Check reference inventory presence; never executes or deletes reference assertions."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
inventory = json.loads((Path(__file__).with_name('reference_behaviors.json')).read_text())
seen = set()
for entry in inventory['entries']:
    if entry['id'] in seen:
        raise RuntimeError('duplicate reference behavior ID')
    seen.add(entry['id'])
    relative = Path(entry['source'])
    if relative.is_absolute() or '..' in relative.parts:
        raise RuntimeError('reference path must stay inside repository')
    source = root / relative
    if not source.is_file() or entry['anchor'] not in source.read_text():
        raise RuntimeError('reference source/anchor missing: ' + str(relative))
    if entry['status'] != 'pending_target_rebuild':
        raise RuntimeError('reference inventory must not claim target behavior passes')
    if not entry['invariant'] or not entry['target_phase'].startswith('p'):
        raise RuntimeError('reference invariant/target phase missing')
print(f'{len(seen)} reference behavior sources/anchors preserved; target rebuild pending')
