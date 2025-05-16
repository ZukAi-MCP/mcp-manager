#!/bin/sh
set -ex
# Install script for a system smileycoin daemon
# Copyright 2018 Jamie Lentin (jamie.lentin@shuttlethread.com)

# This script will install smileycoin globally, create a systemd service for
# it and start the daemon.
#
# It is tested on Debian, but should hopefully work on anything systemd-based.

PROJECT_PATH="${PROJECT_PATH-$(dirname "$(readlink -f "$0")")}"  # The full project path, e.g. /srv/tutor-web.beta
PROJECT_NAME="${PROJECT_NAME-$(basename ${PROJECT_PATH})}"  # The project directory name, e.g. tutor-web.beta
PROJECT_MODE="${PROJECT_MODE-development}"  # The project mode, development or production

SMLY_BIN="${SMLY_BIN-${PROJECT_PATH}/src/smileycoind}"

BINDIR="/usr/local/bin"
TARGETDATA="/var/local/smly"
TARGETCONF="${TARGETDATA}/smileycoin.conf"
TARGETUSER="smly"
TARGETGROUP="nogroup"

# ---------------------------
systemctl | grep -q "smly.service" && systemctl stop smly.service

# The visible smileycoind should be a wrapper to set user/datadir
cat <<EOF > "${BINDIR}/smileycoind"
#!/bin/sh
sudo -u${TARGETUSER} "${SMLY_BIN}" -datadir="${TARGETDATA}" "\$@"
EOF
chown root:root "${BINDIR}/smileycoind"
chmod a+rwx "${BINDIR}/smileycoind"

cat <<EOF > "${BINDIR}/smileycoin-cli"
#!/bin/sh
exec "${PROJECT_PATH}/src/smileycoin-cli" -datadir="${TARGETDATA}" "\$@"
EOF
chown root:root "${BINDIR}/smileycoin-cli"
chmod a+rwx "${BINDIR}/smileycoin-cli"

# install command completion
cp contrib/smileycoin-cli-completion /usr/share/bash-completion/completions/smileycoin-cli

cat <<EOF > "${BINDIR}/smileycoin-walletunlock"
#!/bin/bash
echo -n "Passphrase: "
read -s PASSWD
echo

TIMEOUT="\${1-600}"

exec "${BINDIR}/smileycoin-cli" walletpassphrase "\${PASSWD}" "\${TIMEOUT}"
EOF
chown root:root "${BINDIR}/smileycoin-walletunlock"
chmod a+rwx "${BINDIR}/smileycoin-walletunlock"

grep -qE "^${TARGETUSER}:" /etc/passwd || adduser --system \
    --home "${TARGETDATA}" --no-create-home \
    --disabled-password \
    ${TARGETUSER}
mkdir -p "${TARGETDATA}"
chown -R ${TARGETUSER}:${TARGETGROUP} "${TARGETDATA}"

RPCPASS="$(xxd -ps -l 22 /dev/urandom)"
[ -e "${TARGETCONF}" ] || cat <<EOF > "${TARGETCONF}"
rpcuser=smileycoinrpc
rpcpassword=${RPCPASS}
EOF
cat <<EOF > /etc/systemd/system/smly.service
[Unit]
Description=SMLYcoin
After=network.target

[Service]
ExecStart=${SMLY_BIN} -datadir=${TARGETDATA} --server -printtoconsole -algo=skein
User=${TARGETUSER}
Group=${TARGETGROUP}

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable smly.service
systemctl start smly.service

groupadd -f smly-users
cat <<EOF > /etc/sudoers.d/smly
%smly-users        ALL=(smly) ${SMLY_BIN}
EOF

