#!/bin/bash
# tools/setup-tap.sh — Set up a tap interface for Nyx OS / QEMU
#
# Creates tap0, assigns a private subnet (192.168.100.0/24), enables
# IP forwarding and NAT so the guest can reach the outside world.
# The guest (Nyx OS) uses 192.168.100.2 with gateway 192.168.100.1.
#
# Usage:
#   sudo ./tools/setup-tap.sh up       # create + configure
#   sudo ./tools/setup-tap.sh down     # tear down
#   sudo ./tools/setup-tap.sh          # up, wait for Ctrl+C, then down

TAP_IF=${TAP_IF:-tap0}
TAP_NET=${TAP_NET:-192.168.100}
TAP_IP="${TAP_NET}.1"
GUEST_IP="${TAP_NET}.2"
HOST_WAN=${HOST_WAN:-wlp2s0}   # host interface with internet access

die() { echo "❌ $*" >&2; exit 1; }

up() {
    echo "🔧 Creating TAP interface $TAP_IF ..."

    # Create the tap interface (persistent so it survives QEMU's lifecycle)
    ip tuntap add dev "$TAP_IF" mode tap user "$(whoami)" 2>/dev/null || \
        ip tuntap add dev "$TAP_IF" mode tap 2>/dev/null || \
        die "Cannot create $TAP_IF — are you root?"

    # Bring it up and assign IP
    ip addr add "${TAP_IP}/24" dev "$TAP_IF" 2>/dev/null
    ip link set "$TAP_IF" up

    # Enable IP forwarding
    sysctl -w net.ipv4.ip_forward=1 >/dev/null

    # NAT: masquerade traffic from the tap network out through the WAN
    iptables -t nat -C POSTROUTING -s "${TAP_NET}.0/24" -o "$HOST_WAN" -j MASQUERADE 2>/dev/null || \
        iptables -t nat -A POSTROUTING -s "${TAP_NET}.0/24" -o "$HOST_WAN" -j MASQUERADE

    # Allow forwarding between tap and WAN
    iptables -C FORWARD -i "$TAP_IF" -o "$HOST_WAN" -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$TAP_IF" -o "$HOST_WAN" -j ACCEPT
    iptables -C FORWARD -i "$HOST_WAN" -o "$TAP_IF" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$HOST_WAN" -o "$TAP_IF" -m state --state RELATED,ESTABLISHED -j ACCEPT

    echo "✅ TAP networking ready"
    echo "   Guest IP:  $GUEST_IP"
    echo "   Gateway:   $TAP_IP"
    echo "   DNS:       use your LAN DNS or 8.8.8.8"
    echo ""
    echo "   In Nyx OS shell, run:  net config $GUEST_IP $TAP_IP"
    echo "   Then use the browser!"
}

down() {
    echo "🧹 Cleaning up TAP interface $TAP_IF ..."

    # Remove NAT rules
    iptables -t nat -D POSTROUTING -s "${TAP_NET}.0/24" -o "$HOST_WAN" -j MASQUERADE 2>/dev/null
    iptables -D FORWARD -i "$TAP_IF" -o "$HOST_WAN" -j ACCEPT 2>/dev/null
    iptables -D FORWARD -i "$HOST_WAN" -o "$TAP_IF" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null

    # Delete the tap interface
    ip link set "$TAP_IF" down 2>/dev/null
    ip tuntap del dev "$TAP_IF" mode tap 2>/dev/null

    echo "✅ Cleanup done"
}

case "${1:-}" in
    up)   up   ;;
    down) down ;;
    *)
        up
        echo ""
        echo "── Press Ctrl+C to stop QEMU and tear down tap ──"
        # Wait for Ctrl+C, then tear down
        trap 'echo ""; down; exit 0' INT TERM
        # Wait forever
        while true; do sleep 3600; done
        ;;
esac
