# W522A / W155S1 notes

This tree is an Armbian/mainline-kernel port of the vendor Amlogic W1/W155S1
SDIO Wi-Fi driver.  It was tested on an Amlogic w522a-ap TV box with kernel
`6.12.81-ophub` and GCC 15.

## Practical AP profile

The most stable tested AP profile is:

- interface: `w522a`
- band: 2.4 GHz
- channel: 1
- width: 20 MHz
- mode: 802.11n HT20 safe (`ht_capab=[SHORT-GI-20]`)
- AP-side TX A-MPDU/A-MSDU: disabled by runtime knobs
- U-APSD: disabled
- queue: `txqueuelen=100`, `fq_codel`
- NAT from AP subnet to the current default uplink

This profile is intentionally conservative.  Wider channels and VHT modes can
start, but on the tested box they caused poor latency or full driver/system
hangs.

## Tested results

AP mode, phone client on 2.4 GHz HT20:

- observed ping around 50 ms when the link is healthy
- internet access via NAT worked
- AP download/upload were usable; upload was especially strong in phone
  speed tests
- no fresh kernel `oops`, SDIO reset or hostapd crash was seen after the final
  AP profile was applied

STA mode, same router, tested with `speedtest-cli --source <w522a-ip>`:

- 2.4 GHz SSID: ping 54 ms, download 6.87 Mbit/s, upload 26.46 Mbit/s
- 5 GHz SSID: ping 62 ms, download 4.59 Mbit/s, upload 24.29 Mbit/s
- 5 GHz signal during the test was weak, around -80 dBm

The chip/driver is proprietary-vendor style and not a clean upstream mac80211
driver.  The important practical limitation is that some combinations which
look valid on paper are unstable in real use:

- 5 GHz VHT80 did not reliably start as AP.
- 2.4 GHz HT40 was rejected or unstable.
- legacy 2.4 GHz G mode was worse than HT20 with modern clients.
- forced high HT MCS rates were unstable.
- very small TX queues such as `txqueuelen=8` could hang the driver/system.

## Install

Ready bundle:

```sh
sudo ./w522a-menu.sh
# choose 1
```

Build on target and install:

```sh
sudo ./w522a-menu.sh
# choose 2
```

Direct commands:

```sh
sudo ./w522a-menu.sh install-ready
sudo ./w522a-menu.sh build-install
```

## STA test

The STA test script does not store passwords in the source tree.  Pass them via
environment variables:

```sh
sudo W522A_STA_SSID='your-ssid' W522A_STA_PSK='your-password' \
  /usr/local/sbin/w522a-sta-test.sh
```

Optional BSSID pinning:

```sh
sudo W522A_STA_SSID='your-ssid-5g' W522A_STA_PSK='your-password' \
  W522A_STA_BSSID='aa:bb:cc:dd:ee:ff' /usr/local/sbin/w522a-sta-test.sh
```
