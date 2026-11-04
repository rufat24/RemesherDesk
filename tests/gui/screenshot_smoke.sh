#!/usr/bin/env bash
# Renders a mixed triangle/quad/pentagon mesh in the GUI under Xvfb + Mesa and checks that the screenshot is not blank.
# usage: screenshot_smoke.sh PATH/TO/RemesherDesk MESH PNG_STATS_PY OUT.png
set -euo pipefail
app=$1; mesh=$2; stats=$3; out=$4
rm -f "$out"
xvfb-run -a -s "-screen 0 1280x800x24" "$app" --screenshot "$out" --size 1024 640 "$mesh"
test -s "$out" || { echo "no screenshot was written"; exit 1; }
json=$(python3 -I "$stats" "$out")
echo "$json"
python3 -I - "$json" <<'PY'
import json, sys
s = json.loads(sys.argv[1])
assert s["width"] == 1024 and s["height"] == 640, "unexpected size %r" % s
assert s["unique_colors"] >= 40, "too few colours: the image looks blank or flat"
assert s["nonbackground_fraction"] >= 0.05, "the mesh covers almost nothing"
assert s["stddev"] >= 10, "no contrast"
print("screenshot OK")
PY
