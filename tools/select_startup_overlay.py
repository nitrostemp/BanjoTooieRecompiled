"""Append a demonstrated startup overlay to the selected corpus manifest."""
from pathlib import Path
import argparse,json,tomllib
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('id',type=int)
p.add_argument('--evidence',required=True,help='Path to retained live missing-ID evidence or exact source callsite')
a=p.parse_args()
app=Path(__file__).resolve().parents[1]
all_overlays=tomllib.loads((app.parent/'banjo-tooie/overlays.us.toml').read_text())['overlay']
assert 0<a.id<=len(all_overlays)
row=all_overlays[a.id-1]
assert not row.get('empty'),f'ID {a.id} is an original empty slot'
path=app/'config/startup_coverage.json'
manifest=json.loads(path.read_text());name='.'+row['name']
assert name not in [x['name'] for x in manifest['sections']],f'{name} already selected'
manifest['sections'].append({'name':name,'reason':f'Reached stable ID {a.id}; evidence {a.evidence}'})
path.write_text(json.dumps(manifest,indent=2)+'\n')
print(f'Added ID {a.id}: {name}')
