#!/usr/bin/env bash
# Sets up the community server on a fresh Ubuntu VPS. Run as root:  bash setup-ubuntu.sh
# The server needs Qt 6.10, which only Ubuntu 26.04 has. On an older Ubuntu this script puts a small 26.04 system in
# /opt/u26 and runs the server inside it (RootDirectory in the service); that path needs the prebuilt server
# (a file named driveclubsrv next to this script, built on Ubuntu 26.04 x86-64). On 26.04 itself it compiles from
# source when there is no prebuilt file. Port 8080 (TCP for the menus, UDP for the relay) is the only one opened;
# the admin API stays on the machine.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "Run as root." >&2; exit 1; }

PREBUILT="$(dirname "$(readlink -f "$0")")/driveclubsrv"
. /etc/os-release
RUNTIME_PKGS="libqt6core6t64 libqt6network6 libqt6sql6 libqt6sql6-sqlite libprotobuf32t64"
APP=/opt/driveclub-server
ROOT=""

apt-get update
apt-get install -y ufw
id dcsrv >/dev/null 2>&1 || useradd --system --home "$APP" --shell /usr/sbin/nologin dcsrv

if [ "${VERSION_ID%%.*}" -lt 26 ]; then
  [ -f "$PREBUILT" ] || { echo "Upload the prebuilt server as $PREBUILT first." >&2; exit 1; }
  ROOT=/opt/u26
  apt-get install -y debootstrap
  if [ ! -x "$ROOT/usr/bin/apt-get" ]; then
    # An older debootstrap may not know the 26.04 name: borrow the generic script.
    [ -e /usr/share/debootstrap/scripts/resolute ] || ln -s gutsy /usr/share/debootstrap/scripts/resolute
    debootstrap --arch amd64 --components=main,universe resolute "$ROOT" http://archive.ubuntu.com/ubuntu
  fi
  cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
  mount -t proc proc "$ROOT/proc" 2>/dev/null || true
  chroot "$ROOT" apt-get install -y $RUNTIME_PKGS
  umount "$ROOT/proc" 2>/dev/null || true
elif [ -f "$PREBUILT" ]; then
  apt-get install -y $RUNTIME_PKGS
else
  # Small machines run out of memory while compiling: add swap first.
  if [ "$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)" -lt 3000 ] && ! swapon --show | grep -q .; then
    fallocate -l 3G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
    grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
  fi
  apt-get install -y build-essential cmake ninja-build git qt6-base-dev libqt6sql6-sqlite libprotobuf-dev protobuf-compiler
  [ -d /opt/driveclub-src/.git ] || git clone --branch coop https://github.com/Judaswick/driveclub-server.git /opt/driveclub-src
  # Ubuntu's protobuf has no CMake config file: use the module.
  sed -i 's/find_package(Protobuf CONFIG REQUIRED)/find_package(Protobuf REQUIRED)/' /opt/driveclub-src/CMakeLists.txt
  cmake -S /opt/driveclub-src -B /opt/driveclub-src/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDRIVECLUB_SYSTEM_PROTOBUF=ON
  cmake --build /opt/driveclub-src/build
  PREBUILT=/opt/driveclub-src/build/driveclubsrv
fi

DEST="$ROOT$APP"
mkdir -p "$DEST"
install -m 755 "$PREBUILT" "$DEST/driveclubsrv"

# Settings: keep an existing file, otherwise write one with a random admin key.
if [ ! -f "$DEST/config.json" ]; then
  KEY="$(head -c 24 /dev/urandom | base64 | tr -d '/+=')"
  cat > "$DEST/config.json" <<JSON
{
  "listen": { "address": "0.0.0.0", "port": 8080 },
  "base_path": "",
  "log": { "file": "server.log", "max_size_mb": 50, "trace": false, "trace_body_bytes": 2048 },
  "database": { "file": "driveclub.db", "session_days": 30 },
  "egs": { "server_identifier": "dc-community", "version": "1" },
  "otg": { "base_url": "", "domain_id": "driveclub", "zone": "eu" },
  "shadnet": { "url": "", "client_id": "2a347f19-d4c4-492d-aadc-bc07ae7b8c15", "allow_unverified": true },
  "relay": { "enabled": true },
  "admin": { "address": "127.0.0.1", "port": 8081, "key": "$KEY" }
}
JSON
fi
# A settings file from an earlier run: turn the relay on.
grep -q '"relay"' "$DEST/config.json" || sed -i 's/^  "admin":/  "relay": { "enabled": true },\n  "admin":/' "$DEST/config.json"
chown -R dcsrv:dcsrv "$DEST"
chmod 600 "$DEST/config.json"

{
  echo "[Unit]"
  echo "Description=DriveClub community server"
  echo "After=network-online.target"
  echo "Wants=network-online.target"
  echo
  echo "[Service]"
  echo "User=dcsrv"
  [ -n "$ROOT" ] && { echo "RootDirectory=$ROOT"; echo "MountAPIVFS=yes"; }
  echo "WorkingDirectory=$APP"
  echo "Environment=QT_QPA_PLATFORM=offscreen"
  echo "ExecStart=$APP/driveclubsrv"
  echo "Restart=always"
  echo "RestartSec=3"
  echo "NoNewPrivileges=true"
  echo
  echo "[Install]"
  echo "WantedBy=multi-user.target"
} > /etc/systemd/system/driveclub-server.service
systemctl daemon-reload
systemctl enable driveclub-server
systemctl restart driveclub-server

# Firewall: SSH and the server only. The admin API listens on 127.0.0.1 and is not opened.
ufw allow OpenSSH
ufw allow 8080/tcp
ufw allow 8080/udp   # the relay for the games' race traffic
ufw --force enable

sleep 3
echo "Status: $(systemctl is-active driveclub-server)   Log: $DEST/server.log"
echo "Check from outside: curl -s -o /dev/null -w '%{http_code}' http://THIS-IP:8080/   (404 means it is up)"
