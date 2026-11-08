#!/usr/bin/env bash
# Hardens the VPS after setup-ubuntu.sh. Run as root:  bash harden-ubuntu.sh
# Adds: brute-force protection for SSH, automatic security updates, SSH key login only (only when a key is
# already installed, so you cannot lock yourself out), and a daily copy of the server database (14 kept).
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "Run as root." >&2; exit 1; }

apt-get update
apt-get install -y fail2ban unattended-upgrades sqlite3
systemctl enable --now fail2ban
systemctl enable --now unattended-upgrades

if [ -s /root/.ssh/authorized_keys ]; then
  printf 'PasswordAuthentication no\nPermitRootLogin prohibit-password\n' > /etc/ssh/sshd_config.d/00-keys-only.conf
  sshd -t && systemctl reload ssh
  echo "SSH: password login is off, keys only."
else
  echo "SSH: no key installed in /root/.ssh/authorized_keys, password login left on. Add a key, then run this again."
fi

APP=/opt/driveclub-server
[ -d /opt/u26$APP ] && APP=/opt/u26$APP
cat > /usr/local/bin/driveclub-backup <<SH
#!/bin/sh
mkdir -p /var/backups/driveclub
sqlite3 "$APP/driveclub.db" ".backup '/var/backups/driveclub/driveclub-\$(date +%F).db'"
find /var/backups/driveclub -name 'driveclub-*.db' -mtime +14 -delete
SH
chmod 755 /usr/local/bin/driveclub-backup
printf '17 4 * * * root /usr/local/bin/driveclub-backup\n' > /etc/cron.d/driveclub-backup

echo "Done. fail2ban: $(systemctl is-active fail2ban), updates: $(systemctl is-active unattended-upgrades)"
