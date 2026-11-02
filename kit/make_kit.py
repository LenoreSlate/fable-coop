"""Builds kit/build/FableCoop.bat: the launcher template with proxy/build/dinput8.dll embedded and pinned.
usage: python -I kit/make_kit.py <version>
"""
import base64
import hashlib
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
dll = (root / 'proxy/build/dinput8.dll').read_bytes()
text = (root / 'kit/FableCoop.bat.in').read_text(encoding='ascii')
text = (text.replace('@DLL_SHA@', hashlib.sha256(dll).hexdigest())
            .replace('@DLL_B64@', base64.b64encode(dll).decode())
            .replace('@VERSION@', sys.argv[1]))
out = root / 'kit/build/FableCoop.bat'
out.parent.mkdir(exist_ok=True)
out.write_bytes(text.replace('\r\n', '\n').replace('\n', '\r\n').encode('ascii'))  # cmd wants CRLF
print(out, len(text), 'chars, dll sha256', hashlib.sha256(dll).hexdigest())
