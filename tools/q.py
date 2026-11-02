"""Call one bridge method on a running copy: python -I q.py <profile> <method> [json params]"""
import json
import sys

sys.path.insert(0, 'coop-anything/tools/bridge')
from bridge_client import Bridge, endpoint_from_file

ep = endpoint_from_file('testinstall/Fable Anniversary/CoopProfiles/%s/bridge-endpoint.json' % sys.argv[1])
with Bridge.connect(ep, wait_s=10) as g:
    print(json.dumps(g.call(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}, timeout_s=60)))
