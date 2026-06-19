#!/bin/bash
# W522A 5GHz AP (VHT80, no A-MPDU TX = stable on this firmware) + NAT via wlan0 uplink
# Free the shared radio from BT first (arbiter is exclusive: one radio at a time)
echo serial0-0 > /sys/bus/serial/drivers/hci_uart_aml/unbind 2>/dev/null
# SDIO throttle off (BT unbound = bus not shared) -> full TX speed
for m in /sys/module/aml_sdio/parameters/sdio_write_yield_burst /sys/module/aml_sdio/parameters/sdio_read_yield_burst; do [ -w $m ] && echo 0 > $m; done
ip link set w522a up
ip addr flush dev w522a 2>/dev/null; ip addr add 192.168.50.1/24 dev w522a
killall hostapd dnsmasq 2>/dev/null; sleep 1
hostapd -B /etc/aml-wifi/ap-vht80b.conf
sleep 3
# dnsmasq with PUBLIC upstream DNS (do NOT inherit tailscale MagicDNS resolv.conf)
dnsmasq --interface=w522a --bind-dynamic --except-interface=lo   --no-resolv --server=8.8.8.8 --server=1.1.1.1   --dhcp-range=192.168.50.10,192.168.50.100,12h   --dhcp-option=6,192.168.50.1
sysctl -w net.ipv4.ip_forward=1
nmcli dev set w522a managed no 2>/dev/null || true
iptables -t nat -C POSTROUTING -s 192.168.50.0/24 -o wlan0 -j MASQUERADE 2>/dev/null || iptables -t nat -A POSTROUTING -s 192.168.50.0/24 -o wlan0 -j MASQUERADE
iptables -C FORWARD -i w522a -o wlan0 -j ACCEPT 2>/dev/null || iptables -A FORWARD -i w522a -o wlan0 -j ACCEPT
iptables -C FORWARD -i wlan0 -o w522a -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || iptables -A FORWARD -i wlan0 -o w522a -m state --state RELATED,ESTABLISHED -j ACCEPT
echo 'AP up: SSID=W522A_5G ch36 VHT80, TX MCS6-fixed (~47Mbit dl), DNS=8.8.8.8/1.1.1.1, TX-aggr OFF (firmware bug)'
iptables -C INPUT -p tcp --dport 8080 -j ACCEPT 2>/dev/null || iptables -I INPUT -p tcp --dport 8080 -j ACCEPT
