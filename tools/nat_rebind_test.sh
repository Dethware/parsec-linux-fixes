#!/bin/bash
# End-to-end test for parsec-nat-follow, no root needed:
#
#   unshare -rn tools/nat_rebind_test.sh            # with the helper
#   unshare -rn tools/nat_rebind_test.sh --without  # baseline
#
# Two network namespaces joined by a veth pair. The "host" switches its
# source port 4 s in (NAT rebinding). Without the helper the client stops
# receiving and the host stops hearing from the client; with it, both keep going.
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
peer="$here/nat_rebind_peer.py"
helper="$here/../workaround/parsec-nat-follow"

ip link set lo up
unshare -n sleep 60 &
hostns=$!
sleep 0.3
ip link add v0 type veth peer name v1
ip link set v1 netns "$hostns"
ip addr add 10.99.0.1/24 dev v0
ip link set v0 up
nsenter --preserve-credentials -t "$hostns" -n sh -c \
    'ip link set lo up; ip addr add 10.99.0.2/24 dev v1; ip link set v1 up'

if [ "${1:-}" != "--without" ]; then
    python3 -I "$helper" --local-port 30297 &
    helper_pid=$!
    sleep 0.5
fi

nsenter --preserve-credentials -t "$hostns" -n python3 -I "$peer" host &
host_pid=$!
sleep 0.2
python3 -I "$peer" client
wait "$host_pid"

if [ -n "${helper_pid:-}" ]; then
    kill "$helper_pid"
    wait "$helper_pid" 2>/dev/null || true
fi
kill "$hostns" 2>/dev/null || true
