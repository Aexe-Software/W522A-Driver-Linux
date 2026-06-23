# W522A / W155S1 Driver — Changelog

## v2.3 (2026-06-24) — stable-by-default packaging

Same driver binary as v2.2 (no rate-control / firmware-path changes). This release
makes the **install kit** apply the proven stability fixes automatically and cleans
up packaging. All verified on hardware (kernel 6.12.81-ophub).

- **CPU/IRQ stability layer auto-applied by the installer** (`install.sh prebuilt|build`,
  or `install.sh stability`): `performance` CPU governor (removes the `sugov` RT-kthread
  churn, 24–82 % of a core under SDIO load), `keep_card_irq_masked=1` via `modprobe.d`
  (kills the idle host-wake IRQ storm), and RPS/XPS softirq spreading. Idle CPU ~96 %.
- **Config files now parse.** Base profile `aml_wifi_drv_cfg_0.conf` shipped with
  `key = value` (spaces); `process_drv_cfg_content` does not strip whitespace and
  silently ignored those lines. All shipped profiles are now `key=value` (no spaces).
- **Per-unit RF calibration removed from the repo/release.** Only the generic vendor
  tables (`aml_wifi_rf.txt`, `aml_wifi_rf_fn_link.txt`) ship; firmware auto-selects a
  per-serial `aml_wifi_rf_<SN>.txt` if present, else falls back to generic.
- **Optional RF resilience** (`install.sh resilience`, Wi-Fi-uplink boxes only): SSH
  `ClientAlive` keepalive + a ~0 % CPU gateway link-warm service.
- **Investigated & rejected** `cfg_rx_reorder_timeout` as a loss knob: at 20 ms it
  inflated latency (avg 4→57 ms) with zero loss reduction (head-of-line blocking).
  Stays at default `1`. The few-% 2.4 GHz `ping` loss is ICMP de-prioritisation +
  co-channel collisions (real TCP retransmit ≈ 0 %), not a driver fault — only a clean
  router channel or Ethernet fixes it.

## v2.2 (2026-06-21) — STA stability + channel-width control

Same working WiFi + BT as v2.1, plus STA-mode fixes and a 2.4 GHz width knob. All
changes are already running stably on hardware (kernel 6.12.81-ophub).

**Fixes vs v2.1 (all in `vmac/`):**
- **STA 2.4 GHz channel-width knob** (`wifi_mac_sta.c`): new `sta_2g_ht20` module
  param. `0` (default) = follow the AP / allow HT40 (40 MHz) for peak throughput;
  `1` = force HT20 (20 MHz), more robust on a congested 2.4 GHz band where 40 MHz
  doubles interference exposure and the AP rate-controls the link down to MCS0/1.
  Applies the same clamp the vendor already uses for platform verid 2, *without*
  repurposing verid (which also drives RF cal + minstrel). 5 GHz unaffected.
  Runtime-tunable (`/sys/module/vlsicomm/parameters/sta_2g_ht20`), reconnect to apply.
  Verified: STA associates at 40 MHz (ch6+10), −57 dBm, 0 firmware events under load.
- **STA TX-aggregation restart-freeze gate** (`wifi_mac_if.c`, `wifi_mac_var.h`): new
  `sta_aggr_settle_ms` (default 8000). With STA A-MPDU on, a restart made the first
  post-association burst start ADDBA in the unstable bring-up window → aggregated TX
  wedged the firmware (ADDBA_TIMEOUT → no TX completion → netdev watchdog → 0 Mbit
  while RX/ping still answered). The gate keeps aggregation ENABLED but defers the
  first ADDBA for STA/P2P-client vifs until the link has been connected that long
  (legacy frames first, then aggregate). AP vifs untouched. Read-only
  `stat_addba_deferred` counts skipped attempts. Replaces the blunt `cfg_txaggr=0`.
- **SDIO IRQ governor → hybrid RX poll** (`wifi_sdio.c/.h`, `wifi_hal.c`): v2.1 masked
  the card IRQ by default (`keep_card_irq_masked=1`), which could leave the TX queue
  wedged after boot (that IRQ signals TX completion). v2.2 keeps the HW SDIO IRQ on
  (`keep_card_irq_masked=0`) AND runs a bounded RX poller alongside it (new
  `hybrid_rx_poll=1`, default on); a TX event kicks the poller immediately
  (`aml_sdio_poll_activity()`). Low idle IRQs without the post-boot TX wedge.

---

## v2.1 (2026-06-20) — Idle IRQ storm fixed

Same working WiFi + BT as v2.0, plus a major idle-CPU fix.

**Fix vs v2.0:**
- **Idle SDIO IRQ storm killed.** At idle the chip fired **~1773 SDIO interrupts/sec**, with the `hi_irq` thread eating **8–12% CPU doing nothing**. Root cause traced to two self-inflicted layers:
  1. `hi_irq_task` RX/TX drain loop spinning empty idle polls on every spurious wakeup (`vmac/wifi_hif.c`) — each pass issued a CMD53 status read + `usleep_range`, manufacturing its own SDIO traffic.
  2. The meson-mmc card-IRQ (DAT1) level-storm, independent of driver CMD53 (`vmac/wifi_sdio.c`) — masked, with an hrtimer poller restoring RX/TX cadence.
- Result: **idle interrupts ~1773 → <100/s, idle CPU near-zero**, ping stays clean (0% loss).
- All behaviour is runtime-tunable (no rebuild) under the module's sysfs parameters dir:
  `idle_grace_skip`, `idle_break_on_repeat`, `keep_card_irq_masked`, `poll_interval_us`
  (default 4000 µs) — plus read-only `stat_*` counters (`stat_hi_entry`, `stat_rxtx_entry`,
  `stat_idle_poll`, `stat_idle_break`, `stat_poll_wakes`, …) to measure the storm live.

---

## v2.0 (2026-06-19) — BT + WiFi all work

First fully-working release of the WiFi+BT driver.

**Fixes vs vendor source:**
- Rate control: `get_fitable_mcs_rate()` now caps/floors the AP TX rate to **MCS6**. The vendor reads a hardcoded `sta_avg_snr=28` (never updated), so it would pick the firmware-deadly MCS8/9 (retry storm). AP auto-rate is now fast AND stable — no `iwpriv set_rate_vht` workaround.
- VHT80: added `IEEE80211_VHT_CAP_SHORT_GI_80` to the band VHT cap (`vmac/wifi_cfg80211.c`); without it hostapd rejected VHT80 and the AP was invisible.

**Now working:**
- WiFi AP: 5GHz VHT80 (~60Mbit), 2.4GHz HT40 (~50/50), 2.4GHz HT20 (~26/30).
- WiFi STA both bands (TX aggregation, real bitrate, ~5ms LAN ping).
- **Bluetooth A2DP audio added** (`driver/bt/`): hci_uart aml + bluez-alsa, AAC codec.
- Verified device tree + compiled DTB; new `install.sh` (prebuilt OR build with gcc-15 + autoload); `bt-audio.sh` helper.

**Known firmware limits (traced in code, not driver-fixable):**
- AP TX aggregation (A-MPDU/A-MSDU) crashes the firmware — kept OFF.
- 5GHz 40MHz is firmware-dead (DPD/PA calibration for the 5GHz+40MHz combo). 2.4GHz 40MHz works at the same power.

---

# w522a-v16v вЂ” Changelog

## Current version
`v17m-devin-...+IRQFLAGS-COMMON-BH+SDIO-LIVE-GUARD`

(See `vmac/version.h` for the full tag chain.)

## Local change: +SAFE-AP-TXAMPDU-NON80 (v19k)

**Goal:** undo the global AP TX A-MPDU kill switch without re-enabling the
VHT80 path that previously hard-locked the box.

**Files:**
- `vmac/wifi_mac_if.c`
  - AP TX A-MPDU is now still blocked only when the associated client is on
    80 MHz (`sta_chbw >= WIFINET_BWC_WIDTH80`).
  - For AP mode, outgoing ADDBA requests now advertise a conservative BA
    window of 8 for all channel widths instead of `WME_MAX_BA`.

**Expected behavior:** 2.4G/20, 2.4G/40 and 5G/40 can try AP-side TX A-MPDU
again for better download, while 5G/80 stays in the known-safe RX-BA-only
mode until logs show it can be safely opened.

## Local change: +AP-SERVICE-NAT (v19k)

**Goal:** make the AP useful for direct SSH/web access and keep clients from
treating it as an offline network.

**Files:**
- `local_ap_scripts/w522a-ap-up.sh`
  - Adds optional NAT from the AP subnet to the current default uplink.
  - Applies conservative AP runtime knobs by default: Wi-Fi power save off,
    TX A-MPDU off and TX A-MSDU off through `iwpriv` when available.
- `local_ap_scripts/w522a-ap.default`
  - Default persistent AP profile: `w522a-ap`, 2.4G n20-safe ch1,
    `192.168.77.1/24`.
- `local_ap_scripts/w522a-ap.service`
  - systemd oneshot service for automatic AP startup after boot.
- `w522a-menu.sh`, `local_ap_scripts/w522a-sta-test.sh`, `README-W522A.md`
  - Adds a small install/build menu, a password-free STA speedtest helper and
    tested AP/STA notes for the W155S1 SDIO module.

**Final tested AP runtime:** 2.4 GHz HT20 channel 1, `fq_codel`,
`txqueuelen=100`, NAT enabled, U-APSD off.  Speedtest STA checks were:
2.4 GHz `6.87/26.46 Mbit/s` and 5 GHz `4.59/24.29 Mbit/s` with weak 5G signal.

## Latest change: +IRQFLAGS-COMMON-BH+SDIO-LIVE-GUARD (v17m)

**Goal:** fix the kernel oops seen on the TV screen:
`queued_spin_lock_slowpath -> mmc_release_host -> sdio_release_host ->
aml_w1_sdio_bottom_read`.

**Root cause addressed:**
- `COMMON_LOCK()` used `spin_lock_irqsave()` with the saved IRQ flags stored in
  global `g_hal_priv.com_spinlock_flag`. Two concurrent contexts could overwrite
  this shared flag, so one thread could unlock with the wrong interrupt state and
  exit with IRQs disabled.
- SDIO CMD52/CMD53 helpers could still run while the card was being removed or
  after a function pointer became stale, making `sdio_claim_host()` /
  `sdio_release_host()` unsafe during firmware recovery, rmmod, or card reset.

**Files:**
- `vmac/opt_all.h`
  - `COMMON_LOCK()` / `COMMON_UNLOCK()` now use `spin_lock_bh()` /
    `spin_unlock_bh()` for the shared HAL bookkeeping lock. This preserves
    bottom-half exclusion without storing IRQ flags in a global byte.
- `vmac/w1_sdio/w1_sdio.c`
  - Added `v17m-sdio-guard` checks before sleepable SDIO I/O. Calls from atomic
    context or with IRQs already disabled are dropped and logged instead of
    entering MMC host code.
  - Rejects new SDIO transactions during rmmod/card removal.
  - Uses a weaker safe release helper for already-claimed hosts, so in-flight
    transactions still release the MMC host even after rmmod has started.
  - Re-opens SDIO access only after the final SDIO function probes.

## Latest change: +AUTH-ASSOC-DIAG (v16v)

**Goal:** turn the dark area between *"client tries to connect to the
v16u AP with WPA2-PSK"* and *"`drv_tx_start DROP sta_null=1` two
milliseconds later"* into a path with rate-limited, greppable markers,
so the next dmesg pinpoints exactly which validation check rejects a
modern client (e.g. Realme 10 Android).

**Context (what was reported vs. what is in the source):**

The user's previous log analysis cited log lines like
`v15f auth/assoc: fail on validation! reason=-22` and a function
`aml_mac_add_sta` / `aml_cfg80211_change_sta` вЂ” those exact strings
**do not exist anywhere in the v16u tree** (verified with
`grep -rn` across the entire `vmac/` directory). The legitimate
symptom is the v16u marker `drv_tx_start DROP sta_null=1`
(`wifi_drv_xmit.c` ~line 941) that fires when mac80211 hands a TX
skb whose `nsta` was never populated, i.e. when the AP-side assoc
path failed to register the peer before kernel tried to send to it.

The real AP-side reject paths in v16u are:
- `wifi_mac_recv_auth()` (auth-state handler)
- `wifi_mac_recv_assoc_req()` (assoc-state handler)
- `wifi_mac_parse_counterpart_rsn()` / `wifi_mac_parse_wpa()`
  (RSN/WPA IE parsers that return `WIFINET_REASON_IE_INVALID`)
- `wifi_mac_sta_connect()` (post-validation AID allocation + assoc
  resp TX)

Pre-v16v every one of these used silent macros (`WIFINET_VERIFY_*`)
or debug-flag-gated `WIFINET_DPRINTF` / `ERROR_DEBUG_OUT` prints,
which compile away unless `AML_DEBUG_*` is enabled. Under WPA2 a
client could be deauth'd here without leaving a single line in
dmesg, which is exactly what the user observed.

**Files:**
- `vmac/wifi_mac_recv.c`
  - `wifi_mac_recv_auth()` вЂ” replaced the silent
    `WIFINET_VERIFY_LENGTH` macro with an explicit short-frame check
    that emits `v16v-auth-deny vid=N peer=вЂ¦ reason=short-frame вЂ¦`.
    Added `v16v-auth-rx` entry marker + `v16v-auth-deny` markers at
    the `already-associd`, ACL, and TKIP-countermeasures bail-outs.
  - `wifi_mac_recv_assoc_req()` вЂ” added a single `v16v-assoc-rx`
    entry marker and rate-limited `v16v-assoc-deny` markers at
    EVERY bail-out: `opmode-or-state`, `short-frame`, `bad-bssid`,
    `ie-walk-overrun`, `rates-too-long`, `xrates-too-long`,
    `ssid-too-long`, `ssid-mismatch`, `not-authed`,
    `wpa-rsn-parse-fail` (with full IE diagnostic dump),
    `no-ess-bit`, `short-slot-mismatch`, `11n-only-no-htcap`.
    Added a `v16v-assoc-ok` print right before
    `wifi_mac_sta_connect()` so the next-step boundary is visible.
  - `wifi_mac_parse_counterpart_rsn()` вЂ” every
    `return WIFINET_REASON_IE_INVALID` now emits a
    `v16v-rsn-parse fail: <reason> вЂ¦` print independent of any
    debug-flag gating. Covers: too-short, bad-version,
    bad-mcast-cipher, mcast-cipher-mismatch, ucast-list-too-short,
    bad-ucast-cipher, ucast-cipherset-mismatch,
    keymgmt-list-too-short, bad-akm-cipher, keymgmtset-mismatch,
    no-rsn-cap, pmkid-list-too-short.
  - `wifi_mac_parse_wpa()` вЂ” same treatment with `v16v-wpa-parse`
    prefix (legacy WPA1 IEs).
- `vmac/wifi_mac_sta.c`
  - `wifi_mac_sta_connect()` вЂ” added a `v16v-sta-connect enter`
    pre-allocation marker, a `v16v-sta-connect fail:
    aid-pool-exhausted` marker on the WIFINET_REASON_ASSOC_TOOMANY
    path, a `v16v-sta-connect fail: send_assoc_resp вЂ¦` marker on
    the `wifi_mac_send_mgmt() != 0` path, a `curchan==CHAN_ERR`
    fail marker, and a `v16v-sta-connect ok` success marker. The
    success marker carries the explicit note *"subsequent
    drv_tx_start sta_null=1 would be a TX-path bug, NOT an
    assoc-validation bug"* so the next-iteration log forensics is
    unambiguous.
- `vmac/version.h` вЂ” driver tag bumped to `v16v-devin-AUTH-ASSOC-DIAG+вЂ¦`

**Behavior change:** none. Every new print is `printk_ratelimited`
so a malicious / broken client cannot stall the softirq via flood.
Every existing reject codepath is preserved bit-for-bit; the v16v
marker is added next to the existing one. There is intentionally
**no** "relax / bypass" of any check in this patch вЂ” that would be
a guess until we see which check actually fires.

**How to use the next log:**

After the user reproduces the Realme 10 reject, the dmesg will
contain a self-describing trace, e.g.:

```
v16v-assoc-rx vid=0 opmode=1 state=3 peer=ae:6e:bd:d5:86:77 sub=0x00
v16v-rsn-parse fail: keymgmtset-mismatch peer=0x4 ap=0x2
v16v-assoc-deny vid=0 peer=ae:6e:bd:d5:86:77 reassoc=0 reason=wpa-rsn-parse-fail \
    ie_type=RSN reason_code=17 mc=4/16 uc=4/16 keymgmt=0x4 caps=0x0084 \
    authmode=0 vm_flags=0xвЂ¦ mainsta_flags_ext=0xвЂ¦
```

вЂ¦which tells us in one shot *exactly* which RSN field the client
mismatched. Once that line is visible the v16w patch can apply a
precisely-scoped relaxation (e.g. add WPA2-PSK-SHA256 to the
keymgmt acceptlist) rather than blanket-bypassing validation.



## Latest change: +AP-BW-FIX (v16n)

**Goal:** РІ AP-СЂРµР¶РёРјРµ minstrel РїРµСЂРµСЃС‚Р°С‘С‚ СЃРёРґРµС‚СЊ rate-stats РІ BW_20 РіСЂСѓРїРїРµ
РєРѕРіРґР° РєР°РЅР°Р» HT40/VHT80. РљРѕРЅРєСЂРµС‚РЅРѕРµ РЅР°Р±Р»СЋРґРµРЅРёРµ РЅР° Fn-Link K255B-SR
(W155S1):

```
aml_minstrel_init: sta_chbw=1 fitable_bw=0 init_mcs=7 sta_avg_bcn_rssi=-60 ...
```

`sta_chbw=1` (HT40) в†’ `fitable_bw=0` (HT20) в†’ minstrel_init_start_stats
Р·Р°РїРѕР»РЅСЏРµС‚ СЃС‚Р°С‚РёСЃС‚РёРєСѓ РІ HT20-РіСЂСѓРїРїРµ, Р° TX СЂРµР°Р»СЊРЅРѕ РёРґС‘С‚ РІ HT40 в†’ minstrel
СЂР°Р±РѕС‚Р°РµС‚ СЃ РїСѓСЃС‚С‹РјРё РјРµС‚СЂРёРєР°РјРё РґР»СЏ Р°РєС‚СѓР°Р»СЊРЅРѕР№ РіСЂСѓРїРїС‹ Рё Р·Р°РІРёСЃР°РµС‚ РЅР° РЅРёР·РєРѕРј
MCS / РјР°Р»РµРЅСЊРєРѕРј A-MPDU. РќР° РїСЂРѕР±РЅРѕРј Р·Р°РїСѓСЃРєРµ v16m DL С€С‘Р» РѕРєРѕР»Рѕ 1.7 Mbps
СЃ tx_errв‰€6 %, agg `ampdu_len=4096` (в‰€3 MTU).

**Files:**
- `vmac/rc80211_minstrel_init.c` вЂ” `get_fitable_bw()` С‚РµРїРµСЂСЊ РІ HOSTAP-
  СЂРµР¶РёРјРµ (`sta->sta_wnet_vif->vm_opmode == WIFINET_M_HOSTAP`) СЃСЂР°Р·Сѓ
  РІРѕР·РІСЂР°С‰Р°РµС‚ `sta->sta_chbw`. Р’ AP-СЂРµР¶РёРјРµ `sta_avg_bcn_rssi` РѕС‚РЅРѕСЃРёС‚СЃСЏ
  Рє РќР•РЎРЈР©Р•РЎРўР’РЈР®Р©РРњ Р±РёРєРѕРЅР°Рј РєР»РёРµРЅС‚Р° (Р±РёРєРѕРЅС‹ С€Р»С‘С‚ AP), Рё СЃСЂР°РІРЅРёРІР°С‚СЊ РµРіРѕ
  СЃ `wm_signal_power_bw_change_thresh_narrow/-wide` Р±РµСЃСЃРјС‹СЃР»РµРЅРЅРѕ. Р”Р»СЏ
  STA-СЂРµР¶РёРјР° Р»РѕРіРёРєР° РѕСЃС‚Р°С‘С‚СЃСЏ РїСЂРµР¶РЅРµР№. Р”РѕР±Р°РІР»РµРЅ `pr_info_ratelimited`
  С‡С‚РѕР±С‹ РІРёРґРµС‚СЊ РІ dmesg `rate-init AP: ... using sta_chbw=N directly`.

**Why:** seed `-60` dBm РІ alloc_sta_node() СЂР°Р±РѕС‚Р°РµС‚ РґР»СЏ STA-СЂРµР¶РёРјР°
(СЃРєР°РЅ РјРѕРі РґР°С‚СЊ -90 dBm, РЅРѕ РјС‹ РёСЃРїРѕР»СЊР·СѓРµРј -60 РїРѕРєР° РЅРµ РїСЂРёС€С‘Р» РїРµСЂРІС‹Р№
Р±РёРєРѕРЅ). Р’ AP-СЂРµР¶РёРјРµ `bcn_rssi` РќРРљРћР“Р”Рђ РЅРµ РїСЂРёС…РѕРґРёС‚ вЂ” РєР»РёРµРЅС‚ РЅРµ С€Р»С‘С‚
Р±РёРєРѕРЅС‹ вЂ” РїРѕСЌС‚РѕРјСѓ seed -60 РѕСЃС‚Р°С‘С‚СЃСЏ РЅР°РІСЃРµРіРґР°. Р”Р°Р»СЊС€Рµ СЃ РґРµС„РѕР»С‚РЅС‹РјРё
РїРѕСЂРѕРіР°РјРё `narrow=-74`, `wide=-63` Р»РѕРіРёРєР° get_fitable_bw() РјРѕРіР»Р° РґР°С‚СЊ
Р»СЋР±РѕР№ РёР· BW_20/BW_40/BW_80 РІ Р·Р°РІРёСЃРёРјРѕСЃС‚Рё РѕС‚ С‚РѕРіРѕ РєР°Рє СЃСЂР°РІРЅРёРІР°СЋС‚СЃСЏ
signed int32 c short, Рё РІ РЅР°С€РµРј СЃР»СѓС‡Р°Рµ РґР°РІР°Р»Р° `0` (BW_20) С…РѕС‚СЏ РґРѕР»Р¶РЅР°
Р±С‹Р»Р° РґР°С‚СЊ BW_40 РёР»Рё BW_80 в†’ РѕРіСЂР°РЅРёС‡РµРЅРёРµ РЅР° sta_chbw=1 в†’ 1. Р’РјРµСЃС‚Рѕ
РїРѕРІС‹С€РµРЅРёСЏ РїРѕСЂРѕРіРѕРІ РјС‹ РїСЂРѕСЃС‚Рѕ Р±РёРїР°СЃРёРј РІСЃСЋ beacon-RSSI РІРµС‚РєСѓ РІ AP-СЂРµР¶РёРјРµ вЂ”
СЌС‚Рѕ РµРґРёРЅСЃС‚РІРµРЅРЅС‹Р№ СЃРїРѕСЃРѕР± РєРѕС‚РѕСЂС‹Р№ РјР°С‚РµРјР°С‚РёС‡РµСЃРєРё РєРѕСЂСЂРµРєС‚РµРЅ.

`get_fitable_mcs_rate()` РІ AP-СЂРµР¶РёРјРµ РЈР–Р• РёСЃРїРѕР»СЊР·СѓРµС‚ `sta_avg_rssi`
(RX-RSSI РѕС‚ data-С„СЂРµР№РјРѕРІ), РєРѕС‚РѕСЂС‹Р№ Р’РђР›РР”Р•Рќ РґР»СЏ РєР»РёРµРЅС‚РѕРІ; РїР°С‚С‡РёС‚СЊ РµРіРѕ
РЅРµ РЅР°РґРѕ.

## Previous change: +AP-AMPDU-FULL (v16m)

**Goal:** restore full TX A-MPDU in AP mode so download (AP в†’ client) is no
longer stuck at 1вЂ“6 Mbps.

**Files:**
- `vmac/wifi_drv_xmit.c` вЂ” `drv_tx_prepare()` AP-mode aggregation gate.
  Removed the v16l `cfg_ampdu_subframes <= 4` AP-only condition. AP now
  follows the same TX A-MPDU path as STA mode and the BA-window is purely
  controlled by `cfg_ampdu_subframes` in
  `/etc/aml-wifi/aml_wifi_drv_cfg_0.conf`.
- `vmac/aml_wifi_drv_cfg_0.conf` вЂ” `cfg_ampdu_subframes` bumped from 4 to
  8, `cfg_txamsdu=1`, `cfg_txaggr=1`, `cfg_rxaggr=1`.
- `local_ap_scripts/w522a-ap-up.sh` вЂ” every HT/VHT mode now advertises the
  capabilities that `iw phy phy1 info` already reports as supported on
  W155S1 / Fn-Link K255B-SR: `LDPC`, `TX-STBC`, `RX-STBC1`,
  `MAX-AMSDU-7935`, `SHORT-GI-20`, `SHORT-GI-40` (and `DSSS_CCK-40` on
  2.4 GHz HT40). VHT80 modes also advertise `RXLDPC`,
  `MAX-MPDU-11454`, `RX-STBC-1`, `SU-BEAMFORMEE`, `SHORT-GI-80`.

**Why:** the v16k+`AP-NOAGG`+`AP-HTCAP-SAFE`+`AP-2G-MCS1`+`AP-MCS4-SEED`
band-aid stack disabled TX A-MPDU outright in AP mode and shipped a
hostapd config that advertised only `[SHORT-GI-20]`. Even the v16l
`AP-LITE-AMPDU4` gate kept AP-side A-MPDU disabled unless the runtime
config was capped to 4 sub-frames, which most users never set. The
combined effect was:
- DL (AP в†’ client) PHY rate в‰€ MCS1 14.4 Mbps with **no aggregation**,
  giving 1вЂ“5 Mbps real throughput.
- UL (client в†’ AP) reached only ~5 Mbps because the client also throttled
  itself based on the AP's `[SHORT-GI-20]`-only HT advertisement.

v16m removes the AP suppression and lets `cfg_ampdu_subframes`,
`cfg_ampdu_limit`, `cfg_txamsdu`, `cfg_txaggr` be the only knobs.

## Previous change: +FUNC6-BOUNCE-64K+HW-CRYPTO-DISABLED

**Files:**
- `vmac/w1_sdio/w1_sdio.c` вЂ” FUNC6 RX uses a persistent page-aligned
  fullbounce buffer sized to 64 KB plus PAGE_SIZE slack.
- `vmac/wifi_sdio.c` вЂ” FUNC6 RX mirrors the aligned fullbounce length
  handling for the vlsicomm internal SDIO path.
- `vmac/wifi_hal_com.h` вЂ” `READ_LEN_PER_ONCE` is 64 KB.
- `vmac/wifi_mac_encrypt.c` вЂ” installed HAL keys are forced to
  `HAL_KEY_TYPE_CLEAR` to keep the chip crypto engine out of WPA2.

**What:** Replaces the failed true zero-copy FUNC6 path with page-aligned
fullbounce reads and reduces the read chunk from 96 KB to 64 KB. FUNC4
writes still send the exact caller packet length. Key setup still stores
key material, but the HAL key type is forced clear so hardware crypto is
not selected.

**Why:** On meson-gx-mmc with kernel 6.12, raw FUNC6 buffers with non-zero
`offset_in_page()` fall back from ADMA to PIO and stall the WPA2 handshake.
The 64 KB bounce block avoids PIO while halving the interrupt rate versus
32 KB. Hardware crypto is disabled because WPA2 clients were disconnecting
immediately after EAPOL 4-way completion.

## Previous change: +FUNC6-ZEROCOPY-96K

True zero-copy FUNC6 RX used `buf` directly and aligned only the CMD53
transfer length. Hardware logs then showed `meson-gx-mmc` unaligned sg
offsets such as 508, 492, and 464, so this approach was not viable on
kernel 6.12.

## Previous change: +RXALIGN-V2-32K
Selective FUNC6 RX bounce based on `offset_in_page(buf)` вЂ” page-aligned
destinations zero-copy, misaligned destinations bounce.  Bounce buf
sized to one `READ_LEN_PER_ONCE` (32 KB) plus one PAGE_SIZE of
sdio_align_size slack.  This is what fixed UL throughput from ~5 Mbps
to 350 Mbps.

## Older change: +RX-BOUNCE-OFF

**File:** `vmac/w1_sdio/w1_sdio.c` вЂ” function `aml_w1_sdio_bottom_read()`, FUNC6 branch (around lines 427-450 and the post-read cleanup).

**What:** Reverted the v15v RX-BOUNCE-FIX. In the FUNC6 RX hot path, the
driver now does `kmalloc_buf = buf;` directly вЂ” same approach as the
CoreELEC 1.8.0_20250214 reference driver. No software bounce buffer, no
post-read memcpy, no `__get_free_pages()` per call. Data goes straight
from SDIO into the `hif->rx_fifo.FDB` ring.

**Why:** Under sustained UL load the previous v15v fix forced every
misaligned FUNC6 read (7/8 of all reads, since `rx_fifo_fdt` walks in
PAGE_LEN=512 byte strides and PAGE_SIZE=4096) through a
`memcpy`+`__get_free_pages` path, which pegged the RX data plane at
~5 Mbps on VHT80 (visible in dmesg as `RX FUNC6 bounce hits=N
persistent=N fallback=0`).

**Tradeoff:** On the 6.12 ophub meson-gx-mmc, unaligned FUNC6 reads
fall back to PIO and observation shows the data plane stalls вЂ” STAs
complete the 4-way handshake but `rx_bytes` stays at 0 and they
disassociate within ~5 seconds. To make this change viable, the upper
RX layer needs page-backed skb allocation (so the SDIO host always gets
a page-aligned target), which is not in this tree yet.

## Older inline tags (kept in version.h for traceability)
- `+EXTERN-CLEANUP` вЂ” extern duplicates cleanup
- `+STA-NODE-GUARDS` вЂ” null guards around sta_node access
- `+TX-DRAIN-DIRECT` вЂ” direct TX drain on STA leave
- `+FW-RECOVERY-REAL` вЂ” actual hw reset on fw hang (not just log)
- `+TXAMSDU-ON` вЂ” AMSDU on TX
- `+RX-NICE-5` вЂ” RX kthread nice -5
- `+AP-TX-WATCHDOG` вЂ” AP TX watchdog
- `+DEFRAG-NULL-GUARD` вЂ” defrag null guard
- `+NOTIFIER-LEAK-FIX` вЂ” notifier leak fix on remove
- `+TASKLET-INIT-ONCE` вЂ” tasklet init only once
- `+SCANF-WIDTH` вЂ” sscanf width specifier fix
- `+SDIO-BLKSZ` вЂ” SDIO block size = 512 by default
- `+TRIMODAL` вЂ” trimodal scan/ap/sta state machine
- `+AMPDU-DENSITY` вЂ” AMPDU density tuning
- `+HOSTWAKE-FIX` вЂ” host wake on SDIO
- `+WATCHDOG-FIX` вЂ” watchdog kick fix
- `+RA-SEED-STALE` вЂ” rate-adapter stale seed
- `+RMMOD-HW-RESET` вЂ” hw_reset on rmmod
- `+RX-BOUNCE` вЂ” base RX bounce-fix infrastructure (counters etc.)
- `AB-COMBINED` вЂ” combined branch A/B fixes
