"""Start an isolated server and check its advertised badge enum and PNG uploads.

Python standard library only. No requests go to an existing server. The work
directory is always newly created, with an absolute database path, loopback
listeners, no public listing, and UPnP disabled. Does not launch the game.
"""
import argparse
import base64
from contextlib import contextmanager
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time
import urllib.request
import zlib


def varint(n):
    out = bytearray()
    while n >= 128:
        out.append((n & 127) | 128)
        n >>= 7
    out.append(n)
    return bytes(out)


def field(number, value):
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint((number << 3) | 2) + varint(len(value)) + value


def fields(data):
    pos = 0

    def read_varint():
        nonlocal pos
        value = 0
        for shift in range(0, 70, 7):
            byte = data[pos]
            pos += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
        raise ValueError("oversized varint")

    result = {}
    while pos < len(data):
        tag = read_varint()
        wire = tag & 7
        if wire == 0:
            value = read_varint()
        elif wire == 2:
            size = read_varint()
            value = data[pos:pos + size]
            pos += size
            if len(value) != size:
                raise ValueError("truncated field")
        else:
            raise ValueError(f"unexpected wire type {wire}")
        result[tag >> 3] = value
    return result


def png(size, variant):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    rows = b"".join(b"\0" + bytes((x + variant) % 256 for x in range(size * 4))
                    for _ in range(size))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--schema-check", required=True, type=Path)
    parser.add_argument("--msys2", type=Path, default=Path("C:/msys64"))
    parser.add_argument("--work-root", type=Path, default=Path("out/badge-investigation"))
    args = parser.parse_args()
    server, checker = args.server.resolve(strict=True), args.schema_check.resolve(strict=True)
    args.work_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="server-test-", dir=args.work_root.resolve()))
    port, admin_port = free_port(), free_port()
    while admin_port == port:
        admin_port = free_port()
    config = {
        "listen": {"address": "127.0.0.1", "port": port},
        "admin": {"address": "127.0.0.1", "port": admin_port, "key": "badge-test-only"},
        "database": {"file": str(work / "test.db")},
        "log": {"file": str(work / "server.log"), "trace": True},
        "shadnet": {"url": "", "allow_unverified": True},
        "internet": {"upnp": False},
        "server": {"list_on": "", "name": ""},
    }
    (work / "config.json").write_text(json.dumps(config), encoding="utf-8")
    env = os.environ.copy()
    env["PATH"] = str(args.msys2 / "clang64/bin") + os.pathsep + env.get("PATH", "")
    env["QT_PLUGIN_PATH"] = str(args.msys2 / "clang64/share/qt6/plugins")
    base = f"http://127.0.0.1:{port}"
    # Never use a user's proxy for the isolated loopback server.
    http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    key = b""

    def request(service, method, body=b""):
        signature = base64.b64encode(hmac.digest(
            b"ca91f51f-a7f5-4b95-814a-5796cfff586c", method.encode() + key + body, "sha1"))
        req = urllib.request.Request(base + f"/egs/binary/{service}/{method}", body,
                                     {"Content-Type": "application/octet-stream",
                                      "X-Request-Signature": signature.decode()})
        with http.open(req, timeout=10) as response:
            return response.read(), response.headers

    print(f"Isolated test directory: {work}", flush=True)
    @contextmanager
    def running_server():
        with (work / "console.log").open("ab") as log:
            process = subprocess.Popen([str(server)], cwd=work, env=env, stdout=log,
                                       stderr=subprocess.STDOUT,
                                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            try:
                for _ in range(100):
                    if process.poll() is not None:
                        raise RuntimeError(f"server exited: see {work / 'console.log'}")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise RuntimeError("server did not listen within 10 seconds")
                yield
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)

    def club_metadata(lookup_url=True):
        reply, _ = request("clubService", "queryClubs",
                           field(2, club_id) + field(3, field(1, 1)) +
                           field(7, 1) + field(9, int(lookup_url)))
        return fields(fields(fields(reply)[1])[9])

    def persisted_ids():
        with sqlite3.connect(work / "test.db") as conn:
            row = conn.execute("SELECT customization FROM clubs WHERE id = ?", (club_id,)).fetchone()
        metadata = json.loads(row[0])
        return metadata.get("badgeBobId"), metadata.get("smallBadgeBobId")

    def verify_badge(metadata):
        assert (metadata.get(7), metadata.get(8)) == (full_id, small_id), metadata
        for url_field, bob_id in ((3, full_id), (4, small_id)):
            url = metadata[url_field].decode()
            assert url == f"{base}/ghosts/{bob_id}", url
            with http.open(url, timeout=10) as response:
                assert response.read() == expected_images[bob_id]

    with running_server():
        descriptor, _ = request("indexService", "getMessageDescriptors")
        descriptor_file = work / "advertised-descriptor.bin"
        descriptor_file.write_bytes(descriptor)
        subprocess.run([str(checker), str(descriptor_file)], env=env, check=True)
        login, headers = request("sessionService", "oauthLogin",
                                 field(1, "badge-test") + field(7, "BadgeRegression"))
        key = headers.get("X-SessionKey", "").encode()
        if not key or not fields(login).get(1):
            raise RuntimeError("no login session")
        club, _ = request("clubService", "createAndJoin", field(2, "Badge Test Club"))
        club_id = fields(club)[1]
        assert not {3, 4, 7, 8} & club_metadata().keys()
        assert persisted_ids() == (None, None)
        uploaded_ids = []
        expected_images = {}
        for variant in (0, 1):
            for size in (256, 64):
                data = png(size, variant)
                checksum = base64.b64encode(hashlib.md5(data).digest())
                payload = (field(1, "image/png") + field(2, checksum) +
                           field(3, len(data)) + field(4, data) +
                           field(5, 5) + field(6, 0))
                reply, _ = request("storageService", "createBob", payload)
                result = fields(reply)
                details = fields(result[2])
                if not result[1] or details[1] != result[1] or details[3] != checksum:
                    raise RuntimeError("bad upload ID or checksum")
                url = details[2].decode()
                if not url.startswith(base + "/"):
                    raise RuntimeError(f"unexpected remote download URL: {url}")
                with http.open(url, timeout=10) as response:
                    if response.read() != data:
                        raise RuntimeError("PNG download differs from upload")
                remote, _ = request("storageService", "getRemoteUrl", field(1, result[1]))
                if fields(remote)[2] != details[2]:
                    raise RuntimeError("getRemoteUrl differs from createBob")
                uploaded_ids.append(result[1])
                expected_images[result[1]] = data
                print(f"PASS: {size}x{size} PNG variant {variant}, objectType=5, "
                      f"upload/download/checksum/URL, id={result[1]}", flush=True)
            full_id, small_id = uploaded_ids[-2:]
            customization = field(1, club_id) + field(7, full_id) + field(8, small_id)
            updated, _ = request("clubService", "updateCustomization",
                                 field(1, 1) + field(2, customization))
            verify_badge(fields(fields(updated)[1]))
            verify_badge(club_metadata())
            assert persisted_ids() == (full_id, small_id)
            print(f"PASS: club update and query return both persisted badges {full_id}, {small_id}")
        # A livery-only update must not discard the current badge.
        request("clubService", "updateCustomization",
                field(2, field(1, club_id) + field(2, "test-livery")))
        assert persisted_ids() == (full_id, small_id)
        if len(set(uploaded_ids)) != 4:
            raise RuntimeError("uploads did not receive distinct IDs")

    with running_server():
        assert persisted_ids() == (full_id, small_id)
        verify_badge(club_metadata())
        metadata = club_metadata(lookup_url=False)
        assert (metadata.get(7), metadata.get(8)) == (full_id, small_id)
        assert not {3, 4} & metadata.keys()
    print("PASS: latest full-size and small badges persist across server restart.")



if __name__ == "__main__":
    main()
