import glob
import json
import os
from wot_gamma_paths import CONVERSION

for f in sorted(glob.glob(str(CONVERSION / "*_sky.json"))):
    d = json.load(open(f))
    name = os.path.basename(f).replace("_sky.json", "")
    print(f"{name:12s} {d.get('file') or 'EMPTY'}")
