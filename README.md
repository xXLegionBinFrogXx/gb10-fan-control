# gb10-fan

Experimental NVIDIA GB10 / DGX Spark-class fan control, including Lenovo
ThinkStation PGX, on **Ubuntu/Debian ARM64**. One C executable, `gb10-fan`, provides
manual commands and a foreground temperature governor; the accompanying
`nvfancontrol` kernel driver provides the FF-A/EC transport.
**This is not driverless**: the upstream driver offers only maximum-or-automatic
control, with no way to request a specific RPM.

## Why this exists

**To reduce high idle temperature.** Stock automatic behavior held the machine
within its operating limits; this is not a fix for inadequate or unsafe cooling,
and no defect in the platform's thermal management is claimed or implied. The
motivation is a preference for lower idle temperatures than the stock curve
produces, accepting more fan noise and more fan runtime in exchange. Requesting
a modest RPM floor requires per-RPM control, which the upstream max/auto driver
does not provide.

Treat this as a comfort and preference tool, not a safety or reliability
improvement. If your only concern is staying within operating limits, the stock
behavior already did that and you do not need this software.

Built on [841973620's original driver and reverse engineering](https://github.com/Z841973620/dgx-spark-fan-override),
which made this project possible. See [Transport and attribution](#transport-and-attribution)
for provenance and the changes made here.

This is an independent community project, not affiliated with, endorsed by, or
supported by NVIDIA or Lenovo. Product names are used solely to identify
compatible hardware.

## Disclaimer

**Use this software at your own risk.** Incorrect settings, software defects, or
incompatible hardware or firmware may cause overheating, excessive fan wear,
system instability, hardware damage, or data loss.

This is hardware-experimental software, not production thermal management.
Acknowledgements do not prove fan motion, requested RPM, adequate cooling, or
emergency protection. Monitor the machine independently and reduce/stop workloads
if cooling is uncertain. Neither a maximum-floor request nor firmware protection
is a safety guarantee. Numeric caps can restrict cooling.

### Per-fan RPM figures are unconfirmed

Every per-fan RPM range quoted in this document, including **fan0 1260-9000** and
**fan1 1890-13500**, comes from reading upstream's reverse-engineered EC
capability replies. These figures are **not confirmed** by NVIDIA or Lenovo
documentation, are **not measured** by this software, and are **not verified**
against any fan's actual rating. This project performs no tachometer readback of
any kind: `status` reports the last acknowledged command, never a measured speed.

Consequently the 9000 RPM value used as the automatic policy ceiling, and any
claim that a request "stays inside a fan's rating", rest entirely on an
unverified third-party observation from one machine. They may be wrong for your
hardware, firmware revision, or SKU. A request the EC accepts is not evidence
that a fan can safely sustain it, and a request within these figures is not
evidence of safety either.

**Choosing any RPM value is your decision and your responsibility.** Determine
suitable limits for your own hardware from the manufacturer's specifications and
your own observations. Do not rely on the numbers in this document, or on its
defaults, as a substitute for that.

To the extent permitted by applicable law, this software is provided **"AS IS",
without warranty of any kind**, including warranties of merchantability or
fitness for a particular purpose. Except as required by applicable law or agreed
in writing, the authors, copyright holders, maintainers, and contributors are not
liable for hardware damage, data loss, loss of use, or other damages arising from
the use of, or inability to use, this software.

This notice supplements, and does not replace or modify, the warranty and
liability terms in sections 11 and 12 of [LICENSE](LICENSE). Nothing in this
notice excludes liability that cannot lawfully be excluded.

## Requirements and validation

- Native Ubuntu/Debian `arm64` for Debian packaging and target operation.
- Matching kernel headers, ARM FF-A Direct Request 2 support, and a compatible
  64-bit-mode firmware partition with the verified transport layout below.
- Root for control commands, module management, and installation; `status` can
  run without root when sysfs and temperature sources are readable.
- Optional NVIDIA runtime `libnvidia-ml.so.1`, loaded dynamically for NVML
  temperatures. No CUDA development headers or `nvidia-smi` subprocess is needed.
- Secure Boot/module-signing policy must permit the locally built DKMS module.

On **2026-09-05**, native ARM64 compilation, module loading, package assembly,
and supervised EC exchanges passed on a PGX running `6.17.0-1032-nvidia`.
Checks covered a 6000 RPM floor request, automatic reset, cap rejection, locking,
signal cleanup with a 60-second poll, and missing-sensor maximum-floor behavior.
Both NVML and thermal-zone readings worked. Isolated DKMS builds passed with
DKMS 3.0.11 and 3.4.1 for NVIDIA kernels `6.11.0-1016` and `6.17.0-1032`.
These checks are not tachometer or sustained-load cooling validation. Package
installation and fault injection have not been exercised. Other SKUs/firmware
remain unverified.

On the same host, the idle floor, a `--curve` override, and `max` targeting 9000
were each acknowledged with `fan_fault: 0`, band transitions were observed under
the governor, and SIGINT restored automatic control. Sustained-load cooling and
the missing-sensor path at the reduced 9000 top band have not been re-measured.

## Build

```sh
# Run from the repository root on the target ARM64 host.
sudo apt update
sudo apt install build-essential dpkg-dev linux-headers-$(uname -r) dkms
make
make check
make module
# Alternatively select the matching kernel build tree explicitly:
make module KDIR=/lib/modules/$(uname -r)/build
make deb
```

`make` builds `build/gb10-fan`; `make check` checks compilation and shell syntax
without running a governor or loading a module. `make module` builds
`kernel/nvfancontrol.ko`. `make deb` rebuilds userspace and packages the executable,
DKMS source, systemd unit, README, and license; cross-packaging is unsupported.
DKMS configuration and upgrades require matching headers for **every kernel tree
under `/lib/modules`**, including retained older kernels. Install their headers
as well as those for the running kernel; successful configuration builds for all
of them. Missing headers abort before DKMS removal. Compilation failures stop
configuration, but do not roll back previously removed builds.

## Migrate and install

The upstream package and this release use the same module name, `nvfancontrol`;
the Debian package declares a conflict to prevent co-installation.
Do not run old and new controllers together or assume installing a new module
file replaces the currently loaded module. For a clean machine, skip migration
steps 1-3. For an existing installation:

1. Identify installed old services (`systemctl list-unit-files`), the upstream
   package (`dpkg-query -W nvfancontrol`), and loaded module (`lsmod`). Stop and
   disable each old unit **only if it is installed**:

```sh
sudo systemctl disable --now nvfancontrol.service
sudo systemctl disable --now gb10-fanctl-governor.service
```

2. While the existing module is still loaded, restore automatic control using
   its matching tool and confirm successful acknowledgement/status. For upstream,
   use `sudo nvfancontrol auto` then `nvfancontrol status`; for this release, use
   `sudo gb10-fan auto` then `gb10-fan status`. An older extended controller must
   confirm **both cap and floor** are automatic. Stop any running new governor
   first. Do not unload if restoration fails or transport health is uncertain.
3. Only after confirming automatic control, unload the existing module with
   `sudo modprobe -r nvfancontrol`. Remove the upstream package with
   `sudo apt remove nvfancontrol` **only if installed**. Check `dkms status` and
   remove any known manually installed upstream driver/service using its original
   uninstall procedure. Do not delete unknown DKMS entries or unrelated modules.

**Never reload a module to bypass a latched fault.** The steps above are healthy
migration steps, not fault recovery. A reload loses the diagnostic latch without
establishing what happened at the EC.

Install the new package:

```sh
sudo apt install ./build/gb10-fan_0.2.0_arm64.deb
```

Installation registers/builds the DKMS module and refreshes systemd metadata; it
does **not** load the module or enable, start, or restart either unit. Inspect
`dkms status` and `modinfo -n nvfancontrol` before deliberately activating it:

```sh
sudo modprobe nvfancontrol
gb10-fan status
sudo gb10-fan auto
gb10-fan status
```

Initial status normally reports `unknown` and exits nonzero: probing sends no
commands or EC readback. Explicit `auto` establishes acknowledged automatic state.
Stop on any write failure.
For upgrades, stop the running unit and confirm automatic state before replacing
a healthy loaded driver; package upgrades stop both units but do not restart them
or replace a loaded module. Restart only after deliberate driver activation.
Upgrades preserve your boot-enablement choice; purging removes both units'
enablement links. No old shell configuration is overwritten or evaluated.

## Commands

```text
gb10-fan [options] <command>
status             Cached commands, transport health, and temperature (default)
set <0..13500>     Request floor RPM; 0 disables only the floor
max                Clear cap, then request a 9000 RPM floor
auto               Disable both cap and floor, cap first
cap <1..13500>     Experimental numeric cap; requires both opt-ins below
cap off            Disable only the cap; no opt-in needed
governor           Run in foreground until SIGINT/SIGTERM or failure
--poll <1..60>      Governor interval in seconds (default 5)
--hysteresis <0..10> Downward hysteresis in Celsius (default 3)
--curve <spec>      Floor curve `base,tempC:rpm,...`; overrides the default
--temp-file <path>  Absolute path; exclusive temperature source for status/governor
--allow-cap        Permit numeric cap command in this process
--help, -h, --version
```

`--poll`, `--hysteresis`, and `--curve` apply only to `governor`; `--allow-cap`
only to `cap`.

`set 0` is **not** `auto`: it leaves any cap in place, and does not stop the fans.
Floors and caps are EC target-policy requests, not per-fan speed locks. `max`
removes a potentially restrictive cap first but does not guarantee 9000 RPM or
100% PWM. The two fans can have different physical limits.

`max` requests 9000 RPM, the lowest maximum among the fans' reported capability
ranges (see [Transport and attribution](#transport-and-attribution)), so every
fan stays inside its rating. It is **not** the highest value the EC accepts; use
`set <rpm>` up to 13500 to deliberately request more, understanding that only
fan1 can meet it and that EC behaviour for fan0 above 9000 is unobserved.

`status` reads `fan`, `fan_cap`, and `fan_fault` from the uniquely matching driver
device. These are **last acknowledged commands, not measured fan RPM or current
EC readback**. It exits nonzero for unknown/error state, nonzero/unreadable fault,
missing/incompatible controller, or no valid temperature. Invalid CLI arguments
exit 2; operational failures exit 1. A zero status is not a cooling-health test.

Control commands take `/run/gb10-fan/control.lock`; stop the governor before
manual writes. This advisory lock coordinates **only this executable**. It cannot
exclude direct sysfs writers, old tools, or other firmware mailbox clients.

## Governor and sensors

The governor clears any cap on entry, then requests a floor only when the target
RPM actually changes. The default curve uses the hottest valid temperature:

| Temperature on rise | Requested floor RPM |
| --- | ---: |
| Below 55 C | 2400 |
| 55 to below 65 C | 3200 |
| 65 to below 75 C | 5200 |
| 75 to below 85 C | 8000 |
| 85 to below 92 C | 8500 |
| 92 C and above | 9000 |

A floor is a minimum, not a cap: it never prevents the EC from spinning faster.
The lowest band is a real request rather than a release, so a floor applies at
all times while the governor runs, and the 2400 RPM idle floor is above both
fans' reported minimums. Every default value is at or below 9000, the lowest
maximum among the fans' reported ranges, so automatic policy keeps each fan
inside its rating.
Firmware behavior above the requested floor is not verified here, and per the
disclaimer no floor is a safety guarantee.

Upward changes occur at the threshold on the next poll. Downward changes require
temperature strictly below the crossed threshold minus hysteresis (default 3 C).
Multiple bands can be crossed in one sample.

`--curve <base,tempC:rpm,...>` replaces the default. The first value is the RPM
below the first threshold; each following `tempC:rpm` pair adds a band, using
whole degrees Celsius:

```sh
gb10-fan governor --curve 2400,55:3200,65:5200,75:8000,85:8500,92:9000
```

One to eight pairs are accepted. Temperatures must strictly increase and fall in
20..110 C; RPMs must not decrease and must be 1..13500. Adjacent bands may share
an RPM, which costs no extra EC exchange. An invalid curve is reported and the
process **exits 2 before any privilege check or controller access**, so a broken
override fails at startup instead of running a wrong curve. Values above 9000 are
accepted with a warning, since only fan1 can meet them. `--curve` cannot request
"no floor" for its lowest band; use `set 0` or `auto` for that.

Override the packaged service with a drop-in rather than editing the unit:

```sh
sudo systemctl edit gb10-fan
# [Service]
# ExecStart=
# ExecStart=/usr/sbin/gb10-fan governor --poll 5 --hysteresis 3 --curve 2000,60:4000,75:7000,90:9000
```

By default, temperature is the maximum across available valid NVML GPU readings
and `/sys/class/thermal/thermal_zone*/temp` readings. The NVML group is discarded
if any enumerated GPU cannot be read; valid thermal zones remain usable. Unreadable
or invalid sources are ignored, so a surviving sensor need not cover every hot
component. If **all** temperature sources fail, the governor requests the curve's
top band (9000 RPM by default) and waits for readings to recover, provided the
transport remains usable.

`--temp-file /absolute/path` replaces all automatic sensor discovery. It must
contain an integer in **millidegrees Celsius**, from -40000 through 150000
(for example, `65000` means 65 C). Use a root-trusted file and parent path with a
reliable producer; the program does not enforce ownership or freshness. Never
use an untrusted writable file as a thermal input. Missing/invalid data follows
the same maximum-floor/recovery behavior; stale but valid numbers cannot be detected.

## Services

Two mutually exclusive units are installed. They `Conflicts=` each other and the
two old services, because only one writer may own the controller. Installing the
package does not enable or start either one.

### `gb10-fan-floor.service` (default choice)

A `Type=oneshot` static floor with `RemainAfterExit=yes`. It clears any cap,
requests one RPM floor, and exits; `ExecStop` requests `auto`. Two EC exchanges
per start/stop cycle and no polling process. Firmware performs all escalation
above the floor.

This is the recommended default: it matches the reason this project exists, which
is lowering idle temperature rather than reshaping the whole thermal response.

```sh
sudo systemctl start gb10-fan-floor.service
sudo systemctl enable gb10-fan-floor.service    # only if intended
```

Change the RPM with a drop-in, not by editing the unit:

```sh
sudo systemctl edit gb10-fan-floor.service
```

```ini
[Service]
ExecStart=
ExecStart=/usr/sbin/gb10-fan set 3000
```

Because the floor never changes, nothing reacts to temperature. Whether firmware
escalation above your chosen floor is adequate for your workload is yours to
verify under real load. Read the per-fan disclaimer above before picking a value.

### `gb10-fan.service` (temperature-tracking governor)

Runs the foreground governor, which clears any cap on entry and then tracks the
curve. Choose this if you want RPM to rise with temperature instead of a single
static value. It costs a resident process polling sensors, and an EC exchange on
every band change.

```sh
sudo systemctl start gb10-fan.service
sudo systemctl enable gb10-fan.service          # only if intended
```

There is no shipped configuration file. Use a drop-in:

```ini
[Service]
ExecStart=
ExecStart=/usr/sbin/gb10-fan governor --poll 5 --hysteresis 8
```

SIGINT/SIGTERM cleanup belongs to the governor: it requests `auto` for both
slots, so this unit needs no `ExecStop`.

### Switching between them

```sh
sudo systemctl disable --now gb10-fan.service
sudo systemctl enable --now gb10-fan-floor.service
```

Stopping either unit restores automatic control first, so the machine is never
left holding a floor from a unit that is no longer running. Inspect both with
`systemctl status` and `journalctl -u <unit> -b`. Both use `Restart=no`.

### Caps

Numeric caps require **both** `sudo gb10-fan --allow-cap cap <rpm>` and module
parameter `allow_caps=1`. Permission defaults off and is read-only after load.
Only during a planned, healthy activation with the module not already loaded,
use `sudo modprobe nvfancontrol allow_caps=1`. Adding this argument to an already
loaded module does not change it. Do not reload to evade a fault, persist this
opt-in casually, or enable a unit automatically; both units clear caps on start.

## Failures and retained state

Transport/service errors, timeouts, reply mismatch, or failed shared-buffer
restoration latch `fan_fault` and block further driver requests. The executable
stops after write failure without a blind cleanup/reset/replay. Only a mailbox
busy **before submission**, with no latched fault, is retried (up to three attempts).
Inspect `journalctl -k -b`; stop conflicting writers and investigate the cause.
Do not automatically restart, clear the mailbox, or unload/reload around a fault.

SIGKILL, crashes, stop timeouts, and module unload can leave EC overrides in effect.
Module load/unbind issues no reset; new cached state starts unknown. Overrides are
volatile EC state, but unloading is not a power cycle or a recovery guarantee.
If cooling cannot be established, stop the workload and use the platform's
supported shutdown/recovery procedure, not repeated experimental commands.

## Transport and attribution

Upstream reverse engineering identifies FF-A partition UUID
`884a63a0-3285-4120-83aa-eec008a0a546` (observed as `arm-ffa-17`, ID `0x11`,
properties `0x109`, 64-bit mode), Direct Request 2, eSPI OEM command **17**, and
manifest `ns_shm0` at physical `0x933dd000`, size `0x1000`. Verify firmware/kernel
compatibility rather than editing the UUID/address to force a match.

The 32-byte mailbox frame carries a five-byte `07 <inner> 00 <target LE16>`
request at offset `0x10`; inner `3` writes cap slot `0x119190`, inner `5` floor
slot `0x119192`, and `0xffff` disables a slot. Upstream capability replies report
fan0 1260-9000 RPM and fan1 1890-13500 RPM; these are not this program's telemetry.
The driver requires an idle mailbox, rejects unreserved Linux RAM mappings,
validates service status and the exact reply, polls up to 5 s at 10 ms intervals,
and restores/verifies the initial 32 bytes only after a successful exchange.
These checks do not establish exclusive ownership against unrelated clients.

Derived from [Z841973620/dgx-spark-fan-override](https://github.com/Z841973620/dgx-spark-fan-override),
by **841973620**, whose work supplies the original FF-A/eSPI driver and EC
reverse engineering. **Modified 2026-09-05:** this fork adds dual-slot controls,
transport fault handling, the single C CLI/governor, and packaging. This README
replaces earlier operational/safety guidance. Licensed **GPL-2.0-only**, without
warranty. [LICENSE](LICENSE) reproduces the [upstream license](https://raw.githubusercontent.com/Z841973620/dgx-spark-fan-override/main/LICENSE)
verbatim; its generic application example does not change this project's license.

**Provenance:** the upstream base revision is **unverified**. The original
installer cloned upstream without pinning a revision, and the local import did
not record an upstream commit SHA, so the exact source revision this driver was
derived from cannot be identified. There is no submodule or pinned checkout;
`kernel/nvfancontrol.c` is a vendored, modified copy.
