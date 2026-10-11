#!/usr/bin/env python3
"""Fetch the CC0 props and HDRI in studio-props.json, checking each SHA-256."""
import argparse
import hashlib
import json
import urllib.request
import os
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--work', required=True, help='Parent directory containing assets/ and scenes/')
args = parser.parse_args()
root = Path(args.work).resolve() / 'assets'
for asset in json.loads(Path(__file__).with_name('studio-props.json').read_text()):
    for item in asset['files']:
        target = root / item['path']
        if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == item['sha256']:
            continue
        request = urllib.request.Request(item['url'], headers={'User-Agent': 'SuperLuxCore artist showcase/1.0'})
        target.parent.mkdir(parents=True, exist_ok=True)
        partial=target.with_suffix(target.suffix+'.download')
        digest=hashlib.sha256()
        try:
            with urllib.request.urlopen(request,timeout=90)as response,partial.open('wb')as output:
                while chunk:=response.read(1024*1024):
                    digest.update(chunk);output.write(chunk)
            if digest.hexdigest()!=item['sha256']:
                raise RuntimeError('Source fingerprint mismatch: '+item['path'])
            os.replace(partial,target)
        finally:
            partial.unlink(missing_ok=True)
        print(item['path'])
