"""Compare actual Club Badge uploads, SQLite blobs and HTTP downloads."""

import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
import re
import sqlite3
import urllib.request

from PIL import Image

from test_club_badge_server import fields


def upload_hashes(log):
    requests = {}
    hashes = set()
    checksums = set()
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        start = re.search(r"trace: #(\d+) >>> (.*)", line)
        if start:
            number, route = start.groups()
            requests.pop(number, None)
            if "POST /egs/binary/storageService/createBob " in route:
                requests[number] = bytearray()
        checksum = re.search(r'trace: #(\d+) +~ +checksum: "([^"]+)"', line)
        if checksum and checksum[1] in requests:
            checksums.add(checksum[2])
        chunk = re.search(r"trace: #(\d+) +\| ([a-fA-F0-9]{6})  ([a-fA-F0-9 ]+?)  ", line)
        if chunk and chunk[1] in requests:
            body = requests[chunk[1]]
            if int(chunk[2], 16) == len(body):
                body.extend(bytes.fromhex(chunk[3]))
        end = re.search(r"trace: #(\d+) <<<", line)
        if end and end[1] in requests:
            try:
                upload = fields(requests.pop(end[1]))
                if upload.get(5) == 5 and len(upload[4]) == upload[3]:
                    hashes.add(hashlib.sha256(upload[4]).hexdigest())
            except (IndexError, KeyError, ValueError):
                # Large bodies may be truncated. The upload's MD5 is still logged.
                continue
    return hashes, checksums


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-root", type=Path, default=Path("out/club-badge-game-test"))
    parser.add_argument("--club", type=int, default=1)
    parser.add_argument("--ids", type=int, nargs=2, help="Override the saved full and small IDs")
    parser.add_argument("--client-log", type=Path, help="stderr log from BB_BADGE_PIXELS=1")
    args = parser.parse_args()
    root = args.test_root
    hashes, checksums = upload_hashes(root / "logs/server.log")
    encoded = (args.client_log.read_text(encoding="utf-8", errors="replace")
               if args.client_log else None)
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    failed = False
    db = (root / "server/driveclub.db").resolve()
    with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True) as conn:
        ids = args.ids
        if not ids:
            row = conn.execute("SELECT customization FROM clubs WHERE id = ?", (args.club,)).fetchone()
            metadata = json.loads(row[0])
            ids = [metadata["badgeBobId"], metadata["smallBadgeBobId"]]
        for bob_id, size in zip(ids, (256, 64), strict=True):
            stored, stored_checksum = conn.execute(
                "SELECT data, checksum FROM ghosts WHERE id = ?", (bob_id,)
            ).fetchone()
            with http.open(f"http://127.0.0.1:18080/ghosts/{bob_id}", timeout=10) as response:
                downloaded = response.read()
            digest = hashlib.sha256(stored).hexdigest()
            md5 = base64.b64encode(hashlib.md5(stored).digest()).decode()
            upload_match = "body SHA-256" if digest in hashes else (
                "request MD5" if md5 == stored_checksum and md5 in checksums else "none"
            )
            same_bytes = downloaded == stored and upload_match != "none"
            if encoded is not None:
                same_bytes &= f"BADGE_PIXELS encoded {size}x{size} sha256={digest}" in encoded
            with Image.open(io.BytesIO(downloaded), formats=["PNG"]) as png:
                png.verify()
            with Image.open(io.BytesIO(downloaded), formats=["PNG"]) as png:
                if png.size != (size, size) or png.mode != "RGBA":
                    raise ValueError(f"BOB {bob_id}: unexpected PNG size or mode")
                alpha = png.getchannel("A").histogram()
            visible = sum(alpha[1:])
            print(f"BOB {bob_id} {size}x{size}: {len(downloaded)} bytes, {visible} visible pixels, "
                  f"upload/storage/download match={same_bytes} ({upload_match}), sha256={digest}")
            failed |= not same_bytes or not visible
    raise SystemExit(int(failed))


if __name__ == "__main__":
    main()
