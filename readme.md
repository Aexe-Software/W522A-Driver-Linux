# Amlogic W155S1 / W522A — Working WiFi + Bluetooth Driver

A patched, **actually-working** driver kit for the Amlogic **W155S1 / W522A** SDIO Wi‑Fi + UART Bluetooth combo chip, as found in the **X96 Max Plus W100** TV box (Amlogic S905X3 / SM1), running **kernel `6.12.81-ophub`** (Armbian / ophub, aarch64), built with **GCC 15**.

This is an Armbian/mainline-kernel port of Amlogic's vendor `aml-w1` SDIO driver, with the rate-control, capability and stability bugs fixed so the chip is genuinely usable: **Wi‑Fi Access Point, Wi‑Fi Station, and Bluetooth A2DP audio (AAC)** all work — and every remaining limitation is traced to its exact root cause in the **binary firmware**, not the driver.

> Single-stream (1T1R) 802.11ac. SDIO 3.0 Wi‑Fi + UART Bluetooth that **share one RF front‑end** — only Wi‑Fi **or** Bluetooth can run at a time (hardware radio arbiter on a shared enable GPIO).

---

## ✅ What works

| Mode | Status | Result |
|------|--------|--------|
| **Wi‑Fi AP — 5 GHz VHT80** (ch36) | ✅ stable | ~60 Mbit, primary mode |
| **Wi‑Fi AP — 2.4 GHz HT40** | ✅ stable | ~50 / 50 Mbit |
| **Wi‑Fi AP — 2.4 GHz HT20** | ✅ stable | ~26 / 30 Mbit |
| **Wi‑Fi STA / client** (2.4 & 5 GHz) | ✅ stable | TX aggregation works; real bitrate visible; ~5 ms LAN ping |
| **Bluetooth A2DP audio** | ✅ stable | **AAC** 256 kbps (+ SBC), via bluez‑alsa |

> The earlier conservative "HT20-only" guidance is **obsolete** — VHT80 and 2.4 GHz HT40 are stable with this build thanks to the rate‑control fix below.

## 🪦 What does NOT work — and exactly why (firmware, not driver)

| Limitation | Root cause |
|-----------|-----------|
| **AP TX aggregation** (A‑MPDU / A‑MSDU) | The ARC firmware hard‑hangs a few seconds into aggregated TX *data* → no TX completion → SDIO dies → watchdog reboot. Driver keeps it **off**; RX aggregation works fine (so STA/upload is fast). |
| **5 GHz 40 MHz** (HT40 *and* VHT40) | Firmware DPD/PA calibration for the **5 GHz + 40 MHz** combo is broken: the client associates but the transmitted signal is distorted (chip audibly whines), throughput ≈ 0. The driver programs the channel correctly (verified: center freq, secondary‑offset, bw field), and **2.4 GHz 40 MHz works at the same TX power**, so it is 5 GHz‑RF specific. Binary firmware → not fixable from the driver. |
| **MCS 8–9 (256‑QAM)** | The 1T1R link's SNR can't sustain 256‑QAM; forcing it triggers a retry storm that jams the channel and can wedge the firmware. The driver **caps the AP at MCS6** (see fix #1). |

---

## 🔧 What was fixed vs. the vendor source

1. **AP auto‑rate now actually works.** The vendor rate controller (`vmac/rc80211_minstrel_init.c::get_fitable_mcs_rate`) reads a **hardcoded** `sta_avg_snr = 28` that is never updated, so its SNR gate always permits up to MCS9 — leaving RSSI alone to pick the rate, which on a strong link lands on the firmware‑deadly MCS8/9 (retry storm). Fix: **cap the AP TX rate at MCS6** and floor it to MCS6 on a healthy link, so auto‑rate is both fast *and* stays out of the unstable zone — **no `iwpriv set_rate_vht` workaround needed**.
2. **VHT80 advertises Short‑GI‑80.** Added `IEEE80211_VHT_CAP_SHORT_GI_80` to the band VHT cap in `vmac/wifi_cfg80211.c`; without it `hostapd` rejects the VHT80 config and the AP is **invisible**.
3. **TX aggregation forced OFF in AP mode** — the only stable choice on this firmware.
4. **SDIO fairness throttle disabled** when BT isn't sharing the bus (restores throughput).
5. **Idle SDIO IRQ storm killed (v2.1).** At idle the chip generated **~1773 SDIO interrupts/sec** — the `hi_irq` kernel thread burning **8–12 % CPU for nothing**. Two self‑inflicted layers were traced and fixed: the `hi_irq_task` RX/TX drain loop spinning empty idle polls (`vmac/wifi_hif.c`), and the meson‑mmc card‑IRQ (DAT1) level‑storm (`vmac/wifi_sdio.c`). Result: **idle interrupts ~1773 → <100/s, idle CPU back to near‑zero**, ping stays clean. Every knob is runtime‑tunable under the module's sysfs parameters dir (`idle_grace_skip`, `idle_break_on_repeat`, `keep_card_irq_masked`, `poll_interval_us`), with read‑only `stat_*` counters to verify the effect live.
6. **SDIO IRQ governor refined → hybrid RX poll (v2.2).** The v2.1 storm fix masked the card IRQ by default (`keep_card_irq_masked=1`), but masking it can leave the **TX queue wedged after boot** (the card IRQ is what signals TX completion). v2.2 keeps the hardware SDIO IRQ on (`keep_card_irq_masked=0`) for reliable TX completion **and** runs a bounded RX poller alongside it (new `hybrid_rx_poll=1`, default on) to pick up RX work if the controller drops an in‑band card IRQ — and a TX event kicks the poller immediately (`aml_sdio_poll_activity()` in `vmac/wifi_hal.c`). Keeps idle interrupts low without the post‑boot TX wedge.
7. **STA TX‑aggregation restart‑freeze gate (v2.2).** With STA A‑MPDU on, a box restart made the first post‑association data burst start an ADDBA exchange in the unstable bring‑up window; the aggregated TX then wedged the firmware (ADDBA_TIMEOUT → no TX completion → netdev watchdog → **0 Mbit** while RX/ping still answered). New `sta_aggr_settle_ms` (default **8000 ms**, `vmac/wifi_mac_if.c`) keeps aggregation **enabled** but **defers the first ADDBA** for STA/P2P‑client vifs until the link has been connected that long — legacy frames first (never wedge), then aggregate once the datapath is proven. AP vifs untouched. Read‑only `stat_addba_deferred` counts skipped attempts. (No more blunt `cfg_txaggr=0`.)
8. **STA 2.4 GHz channel‑width knob (v2.2).** New `sta_2g_ht20` module param (`vmac/wifi_mac_sta.c`) controls STA channel width on 2.4 GHz: **`0` (default) = follow the AP, allow HT40 (40 MHz)** for peak throughput; **`1` = force HT20 (20 MHz)** — more robust on a congested band where 40 MHz doubles interference exposure and the AP rate‑controls the link down. Runtime‑tunable (`/sys/module/vlsicomm/parameters/sta_2g_ht20`); reconnect to apply. 5 GHz is unaffected (stays VHT80/40). Verified on hardware: STA associates at **40 MHz** (ch6+10), −57 dBm, 0 firmware events under sustained load, width held under load.

### v2.3 — stable-by-default packaging

9. **CPU/IRQ stability layer applied automatically.** The installer's `prebuilt`/`build` modes now apply the documented stability fixes by default (`./install.sh stability` to re-apply): **`performance` CPU governor** (the default `schedutil` spawns a `sugov` RT kthread that burns 24–82 % of a core under SDIO load), **`keep_card_irq_masked=1`** via `modprobe.d` (kills the idle host-wake IRQ storm, ~12–14 % CPU at idle), and **RPS/XPS** to spread RX/TX softirq across cores. Idle CPU returns to ~96 % idle.
10. **Driver config files now actually parse.** The base profile (`aml_wifi_drv_cfg_0.conf`) shipped with `key = value` (spaces); the parser (`process_drv_cfg_content`) does **not** strip whitespace and silently ignored every spaced line, so the base profile was dead (values happened to match builtins). All shipped profiles are now in the required **`key=value` (no spaces around `=`)** form. Edit them the same way.
11. **Per-unit RF calibration is no longer shipped.** Only the **generic** vendor tables (`aml_wifi_rf.txt`, `aml_wifi_rf_fn_link.txt`) are included. The firmware auto-selects a per-serial `aml_wifi_rf_<SN>.txt` if one exists in `/lib/firmware/aml/`, else falls back to the generic table — correct for most units. (A specific unit's factory cal is device data and is intentionally excluded.)
12. **Optional RF resilience** (`./install.sh resilience`, for boxes whose only uplink is Wi-Fi): SSH `ClientAlive` keepalive so sessions survive transient 2.4 GHz loss bursts, plus a ~0 % CPU gateway link-warm service. Not applied by default (no use on Ethernet).

> **On 2.4 GHz packet "loss".** Investigated exhaustively: on a busy 2.4 GHz channel `ping` to the gateway/Internet shows a few % loss, but that is **ICMP de-prioritisation + ambient co-channel collisions**, not a driver fault — measured real TCP retransmit rate is ~0 %. The one driver timing knob that looked relevant, `cfg_rx_reorder_timeout`, was tested at 20 ms and **made latency far worse (avg 4→57 ms) with no loss reduction** (head-of-line blocking), so it stays at the default `1`. The only real cures are a clean router channel (ch1/ch11) or Ethernet — neither is a driver setting.

### v2.4 — IRQ-storm fix baked into the driver default

13. **`keep_card_irq_masked` now defaults to `1` in the source** (`vmac/wifi_sdio.c`). Previously the default was `0`, so a fresh install with no `modprobe.d` drop-in re-introduced the idle IRQ-23 level storm (~1773 IRQ/s, 9–14 % CPU). The RX path is serviced by the poll timer (and `hybrid_rx_poll`, also default on) and TX completion is caught by the same poller — verified on hardware: a clean install **with no modprobe.d file and no watchdog** boots to **99 % idle**, `keep_card_irq_masked=1` straight from the driver. So the CPU/IRQ fix no longer depends on any external file or service. (The other stability items — CPU governor, RPS, multi-home routing, ARP/rp_filter, default route — are host/network settings a kernel module cannot and should not set, so those remain in `install.sh` / the optional watchdog.)

---

## 📦 Repo layout

```
readme.md
install.sh                One-shot installer (menu): prebuilt OR build-from-source (gcc-15)
driver/
  Makefile
  vmac/                   Wi-Fi MAC / HAL / PHY / rate-control (core driver)
  common/                 Chip register headers
  bt/                     Bluetooth: hci_uart aml driver, bluez-alsa, BT firmware .bin
  firmware/               RF calibration tables (aml_wifi_rf*.txt)
  local_ap_scripts/
    start_ap.sh           Bring up the 5 GHz VHT80 AP + NAT + local speedtest site
    bt-audio.sh           Free the radio from Wi-Fi, bring BT up, pair + play (AAC)
    speedtest_server.py   Local link speedtest web page (measures the Wi-Fi link itself)
    ap-vht80b.conf        hostapd 5 GHz VHT80 config (primary mode)
    ap-ht40.conf          hostapd 2.4 GHz HT40 config
    aml_wifi_drv_cfg_*.conf  driver mode configs (ap / sta / monitor)
  meson-sm1-x96-max-plus-w100-v2.1.dts / .dtb   Device tree (SDIO @ 100 MHz)
  CHANGELOG.md
```

---

## 🚀 Quick install

```sh
sudo ./install.sh
#  1) Install PREBUILT module   (fastest — for kernel 6.12.81-ophub)
#  2) BUILD from source         (installs gcc-15, compiles, installs, loads)
```

Non‑interactive:

```sh
sudo ./install.sh prebuilt     # drop in the prebuilt .ko + configs + autoload
sudo ./install.sh build        # gcc-15 + compile + install + autoload
sudo ./install.sh uninstall
```

The Wi‑Fi driver then auto‑loads on every boot as module **`vlsicomm`** (interface `w522a`).

> ⚠️ **Never `rmmod vlsicomm`** on a running system — its cleanup path doesn't join its kernel threads, so hot‑unload triggers a kernel Oops (a thread executes freed module code) and the box reboots. **To reload, reboot.**

### Device tree

The driver only binds if the board's device tree exposes the SDIO Wi‑Fi function and the BT UART. A verified DTS is in `dtb/` (`meson-sm1-x96-max-plus-w100-v2.1*.dts`):
- Wi‑Fi: `mmc@ffe03000/wifi@1`, `compatible = "amlogic,w155s1"` → `vlsicomm.ko`, SDIO **100 MHz SDR50** (200 MHz SDR104 was unstable under AP load), `host‑wake` IRQ on GPIO 71.
- BT: `serial@24000/bluetooth`, `compatible = "amlogic,w155s2-bt"` → `hci_uart_aml`, `firmware-name = "amlogic/aml_w155s2_bt_uart.bin"`.
- GPIO 71 is the **shared radio enable** (Wi‑Fi reset / host‑wake / BT enable) — this is why Wi‑Fi and BT can't run together; the DTS models it correctly.

---

## 📶 Usage

**Wi‑Fi AP (5 GHz VHT80 — recommended):**
```sh
/root/start_ap.sh        # uses config/ap-vht80b.conf, sets up NAT to the uplink
```
For 2.4 GHz HT40/HT20, edit the hostapd config (`hw_mode=g`, `channel=`, `ht_capab`) and reboot for a clean channel‑width change.

**Wi‑Fi STA / client** (passwords passed via env, never stored in the tree):
```sh
sudo W522A_STA_SSID='your-ssid' W522A_STA_PSK='your-pass' \
  wpa_supplicant -B -i w522a -c /tmp/sta.conf -D nl80211   # add bgscan="" in the network block
sudo dhcpcd w522a
```
> For low STA latency keep the interface NetworkManager‑unmanaged and set `bgscan=""`, otherwise periodic scans make the single radio go off‑channel (~2 s ping spikes).

STA **2.4 GHz channel width** (see fix #8) — default is 40 MHz; force 20 MHz for a congested band:
```sh
# 1 = force HT20 (robust), 0 = allow HT40/40 MHz (default). Reconnect to apply.
echo 1 | sudo tee /sys/module/vlsicomm/parameters/sta_2g_ht20
# persist across reboot:
echo 'options vlsicomm sta_2g_ht20=1' | sudo tee /etc/modprobe.d/w522a-ht20.conf
```

**Bluetooth A2DP audio** (shares the radio — Wi‑Fi must be off first):
```sh
sudo ./scripts/bt-audio.sh up               # disables Wi-Fi autoload (reboot once), then frees radio + BT up + bluealsa(AAC)
sudo ./scripts/bt-audio.sh pair AA:BB:CC:DD:EE:FF
sudo ./scripts/bt-audio.sh play AA:BB:CC:DD:EE:FF song.mp3
sudo ./scripts/bt-audio.sh wifi-back         # hand the radio back to Wi-Fi (reboot)
```

---

## ⚠️ Operational landmines (learned the hard way)

- **Never `rmmod vlsicomm`** → kernel Oops + reboot. Reboot to reload the driver.
- **Wi‑Fi ⟷ BT share one radio** — they can't run simultaneously. Switch with `scripts/bt-audio.sh`.
- **Changing AP channel width on a live driver** corrupts the station table (auth loops). Change the config, then **reboot**.
- A 2.4 GHz AP overlaps a 2.4 GHz USB uplink dongle (if you use one) — prefer the 5 GHz AP to avoid contention.

---

## Hardware / build environment

- Box: **X96 Max Plus W100** (Amlogic **S905X3 / SM1**, 4 GB/32 GB)
- Combo: **W155S1 / W522A** (Fn‑Link), 1T1R 802.11ac + BT
- Kernel: **6.12.81‑ophub** (Armbian / ophub) · Toolchain: **GCC 15**

## Credits & License

The driver core derives from Amlogic's vendor `aml-w1` source (their license applies to those files). Bluetooth audio uses **[bluez‑alsa](https://github.com/arkq/bluez-alsa)**. Firmware blobs are Amlogic property, bundled for convenience. The fixes, integration, install/BT scripts and the verified device tree are provided here.

Issues and contributions welcome — to date this is the only known **fully working** build for this chip.
