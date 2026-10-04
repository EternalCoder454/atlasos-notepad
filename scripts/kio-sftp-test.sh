#!/bin/bash
# KIO against a real sftp server, in the dev container:
#   scripts/dev.sh scripts/kio-sftp-test.sh build/kio
# Starts sshd on 127.0.0.1:2222 (temp host key, one ed25519 key, temp HOME),
# runs app_test with NP_KIO_TEST_BASE set so the sftp tests run, then opens an
# sftp URL in the app itself under Xvfb. Stops everything it started, even on
# failure.
set -uo pipefail

# It adds a user and writes root's ~/.ssh: never outside a container, and
# not in a toolbox (which shares the host's home and more).
if [ ! -e /run/.containerenv ] || [ -e /run/.toolboxenv ] || [ "$(id -u)" != 0 ]; then
    echo "kio-sftp-test: run this through scripts/dev.sh (a throwaway container, as root)" >&2
    exit 2
fi

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$(cd "${1:?usage: kio-sftp-test.sh <build dir>}" && pwd)
need=()
for pkg in openssh-server openssh-clients kio-extras xorg-x11-server-Xvfb; do
    rpm -q "$pkg" >/dev/null 2>&1 || need+=("$pkg")
done
if [ "${#need[@]}" -gt 0 ]; then
    dnf -y install "${need[@]}" >&2 || { echo "kio-sftp-test: can't install ${need[*]}" >&2; exit 2; }
fi

work=$(mktemp -d /var/tmp/np-sftp.XXXXXX)
sshd_pid=
made_user=
ssh_dir=/nonexistent-never-removed
# shellcheck disable=SC2329 # called by the trap below
cleanup() {
    [ -n "$sshd_pid" ] && kill "$sshd_pid" 2>/dev/null
    pkill -f "sshd.*$work" 2>/dev/null
    [ -n "$made_user" ] && pkill -u np-sftp 2>/dev/null
    # What the runs left behind (a KIO worker, kiod, a bus): only processes
    # started with this run's XDG_RUNTIME_DIR, never this shell.
    local p
    for p in /proc/[0-9]*; do
        [ "${p#/proc/}" = "$$" ] && continue
        grep -qzxF "XDG_RUNTIME_DIR=$work/run" "$p/environ" 2>/dev/null &&
            kill -KILL "${p#/proc/}" 2>/dev/null
    done
    sleep 1
    [ -n "$made_user" ] && userdel -r np-sftp >/dev/null 2>&1
    local _
    for _ in 1 2 3; do
        rm -rf "$work" "$ssh_dir" 2>/dev/null && [ ! -e "$work" ] && break
        sleep 1
    done
}

mkdir -p "$work/server" "$work/files" "$work/xdg/config" "$work/xdg/data" "$work/xdg/cache" "$work/xdg/state" "$work/run"
chmod 700 "$work/run"
# ssh and libssh find ~/.ssh through the passwd entry, not $HOME, so the keys
# go in the real one. This is a throwaway container (dev.sh uses --rm): it
# refuses to touch an .ssh that has anything in it.
home=$(getent passwd "$(id -u)" | cut -d: -f6)
ssh_dir="$home/.ssh"
if [ -n "$(ls -A "$ssh_dir" 2>/dev/null)" ]; then
    echo "kio-sftp-test: $ssh_dir isn't empty; run this in the dev container only" >&2
    exit 2
fi
mkdir -p "$ssh_dir"
chmod 700 "$ssh_dir"
trap cleanup EXIT INT TERM
export HOME="$home"
# sshd (without PAM) won't let a locked account in, and root's is: the server
# side runs as its own user, in the same file system as the tests.
useradd -m -s /bin/bash -p '*' np-sftp || { echo "kio-sftp-test: can't add the np-sftp user" >&2; exit 2; }
made_user=1
chmod 755 "$work"; chmod 777 "$work/files"
ssh-keygen -q -t ed25519 -N '' -f "$ssh_dir/id_ed25519"
ssh-keygen -q -t ed25519 -N '' -f "$work/server/host_key"
cp "$ssh_dir/id_ed25519.pub" "$work/server/authorized_keys"
# sftp-server logs every open/rename/remove to its stderr, which sshd would
# send to the client: keep it in a file (to see whether a save goes through
# a .part file and a rename).
cat > "$work/server/sftp-logged" <<WRAP
#!/bin/sh
exec /usr/libexec/openssh/sftp-server -l VERBOSE -e 2>> "$work/sftp.log"
WRAP
chmod 755 "$work/server/sftp-logged"
: > "$work/sftp.log"
chmod 666 "$work/sftp.log"

cat > "$work/server/sshd_config" <<CFG
Port 2222
ListenAddress 127.0.0.1
HostKey $work/server/host_key
PidFile $work/server/sshd.pid
AuthorizedKeysFile $work/server/authorized_keys
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PermitRootLogin yes
Subsystem sftp $work/server/sftp-logged
LogLevel VERBOSE
CFG
mkdir -p /run/sshd
/usr/sbin/sshd -D -e -f "$work/server/sshd_config" 2> "$work/sshd.log" &
sshd_pid=$!
for _ in $(seq 50); do
    (exec 3<>/dev/tcp/127.0.0.1/2222) 2>/dev/null && break
    sleep 0.2
done
# The host key is known, so KIO has nothing to ask about.
ssh-keyscan -p 2222 127.0.0.1 2>/dev/null | sed 's/^127.0.0.1/[127.0.0.1]:2222/' > "$ssh_dir/known_hosts"
cat > "$ssh_dir/config" <<CFG
Host 127.0.0.1
    User np-sftp
    Port 2222
    IdentityFile $ssh_dir/id_ed25519
    UserKnownHostsFile $ssh_dir/known_hosts
    StrictHostKeyChecking yes
CFG
chmod 600 "$ssh_dir/config"
ssh -v -o BatchMode=yes -p 2222 np-sftp@127.0.0.1 true 2> "$work/ssh.log"
echo "ssh login to the test sshd: exit $?"
[ -s "$work/ssh.log" ] && grep -E "Authenticat|denied|refused|Permission|Host key|known_hosts" "$work/ssh.log" | head -6

export XDG_CONFIG_HOME="$work/xdg/config" XDG_DATA_HOME="$work/xdg/data" XDG_CACHE_HOME="$work/xdg/cache" XDG_STATE_HOME="$work/xdg/state" XDG_RUNTIME_DIR="$work/run"
export NP_KIO_TEST_BASE="sftp://np-sftp@127.0.0.1:2222$work/files"
export QT_QPA_PLATFORM=offscreen
status=0

echo "== app_test over sftp"
# umask 000 for the test process only: the files it makes are writable for
# the server's user too.
(umask 000; dbus-run-session --config-file="$here/../apps/atlas-notepad/tests/dbus-session.conf" -- timeout -s KILL 200 "$build/app_test" kioRoundTrip sftpRoundTrip sftpConflictMissingAndReadOnly kioRefusals kioConflictAndFailure kioCancelFirstOpenLeavesNoClosedTab kioSaveRaisesNoBanner relocateDuringRemoteSave renameDuringRemoteLoad) > "$work/app_test.log" 2>&1
test_rc=$?
grep -E "^(PASS|FAIL|SKIP|Totals|   Loc|   Actual|   Expected)" "$work/app_test.log"
[ "$test_rc" = 0 ] || { status=1; echo "app_test exit $test_rc"; tail -15 "$work/app_test.log"; tail -15 "$work/sshd.log"; }

echo "== what sshd's sftp-server did with the test files (does a save go through a .part and a rename?)"
grep -E 'open "|rename|remove|posix-rename' "$work/sftp.log" | sed 's/^.*sftp-server\[[0-9]*\]: //' | sort | uniq -c | head -20
echo "== the app itself, under Xvfb, opening an sftp URL"
printf 'from the server\n' > "$work/files/e2e.txt"
sessions_before=$(grep -c "subsystem 'sftp'" "$work/sshd.log")
: > "$work/app.log"
(
    export QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software QT_FORCE_STDERR_LOGGING=1 QT_LOGGING_RULES='kf.kio*=true'
    unset WAYLAND_DISPLAY
    # The bus inside the display, so the services KIO starts (kpasswdserver)
    # are born with it.
    xvfb-run -a -s "-screen 0 1920x1080x24" dbus-run-session -- \
        timeout -s TERM 25 "$build/atlas-notepad" "sftp://np-sftp@127.0.0.1:2222$work/files/e2e.txt" > "$work/app.log" 2>&1
)
# Only the app's and KIO's own lines: the container's portal daemons grumble
# about having no desktop.
grep -iE "atlas-notepad|qrc:|kf\.kio|kioworker|sftp|Main\.qml|QML|qt\." "$work/app.log" | grep -iE "error|fail|critical|warn|cannot|can't" | grep -v "fuse init" | grep -v xdg-desktop-portal > "$work/app.errors"
echo "app log lines with errors or warnings: $(wc -l < "$work/app.errors")"
head -10 "$work/app.errors"
if [ "$(grep -c "subsystem 'sftp'" "$work/sshd.log")" -gt "$sessions_before" ]; then
    echo "sshd saw the app's own sftp session: yes"
else
    echo "sshd saw the app's own sftp session: NO"; status=1
    tail -12 "$work/sshd.log"; echo "-- app log"; tail -12 "$work/app.log"
fi
[ -s "$work/app.errors" ] && status=1
exit $status
