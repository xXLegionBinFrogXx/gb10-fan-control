# gb10-fan-control

Experimental NVIDIA GB10 / DGX Spark-class fan control on **Ubuntu/Debian ARM64**.
Tested on **Lenovo ThinkStation PGX** and **NVIDIA DGX Spark**.

One C executable, `gb10-fan`, provides manual commands and a foreground
temperature governor. The `nvfancontrol` kernel driver provides the FF-A/EC
transport. **This is not driverless:** the upstream driver only offers
maximum-or-automatic control. There is no way to request a specific RPM without
this (or equivalent) module.

This is an independent community project, not affiliated with, endorsed by, or
supported by NVIDIA or Lenovo. Product names identify compatible hardware only.

Built on [841973620's original driver and reverse engineering](https://github.com/Z841973620/dgx-spark-fan-override).
See [Transport and attribution](#transport-and-attribution).

## Why this exists

Stock automatic control already held tested machines within operating limits.
This project does **not** claim a platform defect.

The goal is a **lower idle temperature** than the stock curve, in exchange for
more fan noise and more fan runtime. That needs a modest RPM floor, which the
upstream max/auto interface cannot request.

Treat this as a comfort tool, not a safety or reliability improvement. If
staying inside operating limits is enough, you do not need this software.

## Risks

**Use this at your own risk.** Bad settings, bugs, or mismatched
hardware/firmware can overheat the machine, wear fans, destabilize the system,
or damage hardware.

- Acknowledgements are **not** proof of fan motion, requested RPM, adequate
  cooling, or emergency protection.
- `status` reports the **last acknowledged command**, never measured speed.
  This project has **no tachometer readback**.
- Per-fan ranges quoted here (**fan0 1260–9000**, **fan1 1890–13500**) come from
  upstream EC capability replies on one class of machine. They are **not**
  vendor specs, **not** measured here, and **may be wrong** for your SKU or
  firmware. A request the EC accepts is not evidence the fan can sustain it.
- Floors are minima. Caps can **restrict** cooling. Choosing any RPM is your
  decision; use manufacturer data and your own observation, not these defaults.
- Monitor independently. Reduce or stop workloads if cooling is uncertain.

Provided **"AS IS"**, without warranty. See [LICENSE](LICENSE) §§11–12. This
notice does not replace that license or exclude liability that cannot lawfully
be excluded.

## Power cycle before you chase software

On **3 of 4** tested machines (PGX and DGX Spark), fan overrides did not take
effect until a **full power cycle**. Warm reboot, firmware-setup defaults,
driver purge/reinstall, kernel change, Secure Boot change, and module reload
did **not** clear it.

Typical symptoms after a fresh `modprobe`:

- First EC exchange fails with firmware service status `5`, or
- Exchanges succeed (`fan_fault: 0`, byte-exact replies) but the fans stay on
  the stock curve and ignore every override.

**Do this once on a new or newly misbehaving machine**, using the vendor power-off
procedure:

1. Shut down cleanly.
2. Disconnect AC.
3. Drain standby power (hold the power button ~10 s).
4. Reconnect and boot.
5. Load the module, run `gb10-fan auto`, then a floor request, and confirm the
   fans actually change — not only that `status` shows an acknowledgement.

The firmware-side cause is unidentified. This is an observed remedy, not a
guaranteed or explained recovery. After any latched fault, save
`journalctl -k -b` **before** touching the module. Reloading clears the
in-memory latch and loses nothing useful; it is not a fix.

**Never reload the module to bypass a latched fault.**

## Quick start (clean machine)

Native ARM64 host, matching kernel headers, root for control/install.
`status` can run unprivileged when sysfs and temperature sources are readable.

```sh
sudo apt update
sudo apt install build-essential dpkg-dev linux-headers-$(uname -r) dkms
make
make check
make module          # or: make module KDIR=/lib/modules/$(uname -r)/build
make deb
sudo apt install ./build/gb10-fan_0.2.0_arm64.deb
```

The package registers the DKMS module and refreshes systemd. It does **not**
load the module or enable/start either unit.

```sh
sudo modprobe nvfancontrol
gb10-fan status                 # unknown / nonzero is normal before any write
sudo gb10-fan auto
gb10-fan status
sudo systemctl start gb10-fan-floor.service
sudo systemctl enable gb10-fan-floor.service   # only if you want it at boot
```

Stop on any write failure. If acknowledgements look fine but the fans do not
move, do the [power cycle](#power-cycle-before-you-chase-software) before
debugging the CLI.

Migrating from the upstream `nvfancontrol` package: see
[Migrate](#migrate-from-upstream).

## Choose one writer

Two mutually exclusive units. Installing the package enables neither.
`Conflicts=` covers each other and the old upstream / governor units. Only one
writer may own the controller.

| Unit | What it does | Use when |
| --- | --- | --- |
| `gb10-fan-floor.service` | Oneshot: clear cap, set one floor, exit. `ExecStop` → `auto`. | Default. Lower idle temp; firmware still escalates above the floor. |
| `gb10-fan.service` | Resident governor; curve tracks temperature. Cleanup is in-process (`auto` on SIGINT/SIGTERM). | You want RPM to rise with temperature. |

Both use `Restart=no`. Stopping either unit restores automatic control first.

### Static floor (recommended)

```sh
sudo systemctl start gb10-fan-floor.service
sudo systemctl enable gb10-fan-floor.service
```

Change RPM with a drop-in, not by editing the unit:

```sh
sudo systemctl edit gb10-fan-floor.service
```

```ini
[Service]
ExecStart=
ExecStart=/usr/sbin/gb10-fan set 3000
```

The floor never changes with temperature. Whether firmware escalation above it
is enough for your workload is yours to verify under load.

### Governor

```sh
sudo systemctl start gb10-fan.service
sudo systemctl enable gb10-fan.service
```

```ini
# sudo systemctl edit gb10-fan.service
[Service]
ExecStart=
ExecStart=/usr/sbin/gb10-fan governor --poll 5 --hysteresis 8
```

Switch:

```sh
sudo systemctl disable --now gb10-fan.service
sudo systemctl enable --now gb10-fan-floor.service
```

## Commands

```text
gb10-fan [options] <command>
status                 Cached commands, transport health, temperature (default)
set <0..13500>         Request floor RPM; 0 disables only the floor
max                    Clear cap, then request a 9000 RPM floor
auto                   Disable cap first, then floor
cap <1..13500>         Experimental numeric cap; needs both opt-ins below
cap off                Disable only the cap
governor               Foreground until SIGINT/SIGTERM or failure
--poll <1..60>         Governor interval in seconds (default 5)
--hysteresis <0..10>   Downward hysteresis in Celsius (default 3)
--curve <spec>         Floor curve `base,tempC:rpm,...`
--temp-file <path>     Absolute path; exclusive temperature source
--allow-cap            Permit numeric cap in this process
--help, -h, --version
```

`--poll`, `--hysteresis`, and `--curve` apply only to `governor`.
`--allow-cap` applies only to `cap`.

| Pitfall | Actual meaning |
| --- | --- |
| `set 0` | Not `auto`. Leaves any cap in place; does not stop the fans. |
| `gb10-fan max` | Clears cap, then floor **9000** (lowest reported fan maximum). |
| sysfs `fan` = `max` | Floor **13500**, cap **unchanged**. Different command. |
| Floor / cap | EC target-policy requests, not per-fan speed locks. |
| `status` | Last ack + `fan_fault`. Exit 0 is not a cooling-health test. |

`max` at 9000 is **not** the highest value the EC accepts. `set` up to 13500 is
allowed; only fan1 is reported able to meet it, and fan0 above 9000 is
unobserved. Neither interface guarantees the requested speed.

Control commands take `/run/gb10-fan/control.lock`. Stop the governor before
manual writes. The lock coordinates **this executable only**. It cannot exclude
direct sysfs writers, old tools, or other firmware mailbox clients.

Invalid CLI arguments exit 2. Operational failures exit 1. `status` exits
nonzero for unknown/error state, nonzero/unreadable fault, missing controller,
or no valid temperature.

### Caps

Numeric caps need **both**:

1. Module loaded with `allow_caps=1` (read-only after load; default off)
2. `sudo gb10-fan --allow-cap cap <rpm>`

Only when the module is **not** already loaded:

```sh
sudo modprobe nvfancontrol allow_caps=1
```

Do not persist this casually or enable a unit that applies a cap at boot. Both
shipped units clear caps on start.

## Governor and sensors

On start the governor clears any cap, then writes a floor only when the target
RPM changes. Default curve, hottest valid temperature:

| Temperature on rise | Requested floor RPM |
| ------------------- | ------------------: |
| Below 50 °C         |                2400 |
| 50 to below 65 °C   |                3200 |
| 65 to below 75 °C   |                5200 |
| 75 to below 85 °C   |                8000 |
| 85 to below 92 °C   |                8500 |
| 92 °C and above     |                9000 |

A floor never prevents the EC from spinning faster. The lowest band is a real
request, so a floor is present the whole time the governor runs. Defaults stay
at or below 9000. Firmware behavior above the floor is not verified here.

Upward changes apply at the threshold on the next poll. Downward changes need
temperature strictly below `threshold − hysteresis` (default 3 °C). One sample
may cross several bands.

```sh
gb10-fan governor --curve 2400,50:3200,65:5200,75:8000,85:8500,92:9000
```

- One to eight `tempC:rpm` pairs after the base RPM.
- Temperatures strictly increasing, 20–110 °C; RPMs non-decreasing, 1–13500.
- Adjacent bands may share an RPM (no extra EC write).
- Invalid curve → exit 2 **before** privilege or controller access.
- RPM above 9000 is accepted with a warning.
- The lowest band cannot be “no floor”; use `set 0` or `auto` for that.

Default temperature is the max of valid NVML GPU readings and
`/sys/class/thermal/thermal_zone*/temp`. The whole NVML group is discarded if
any enumerated GPU cannot be read; thermal zones can still be used. If **all**
sources fail, the governor requests the curve top band (9000 by default) and
waits for recovery, provided the transport still works.

`--temp-file /absolute/path` replaces discovery. File must hold millidegrees
Celsius, integer −40000..150000 (`65000` = 65 °C). Use a root-trusted path.
Missing/invalid data follows the same maximum-floor path; stale-but-valid
numbers cannot be detected.

Optional NVML via `libnvidia-ml.so.1` (dlopen). No CUDA headers and no
`nvidia-smi` subprocess.

## Build notes

`make` → `build/gb10-fan`. `make check` compiles and checks shell syntax; it
does not run a governor or load a module. `make deb` rebuilds userspace and
packages the executable, DKMS source, systemd unit, README, and license.
Cross-packaging is unsupported.

DKMS configuration builds for **every** kernel tree under `/lib/modules`.
Install headers for retained older kernels too. Missing headers abort before
DKMS removal. A compile failure stops configuration but does not roll back
already-removed builds.

Secure Boot / module-signing policy must allow the locally built DKMS module.
Need ARM FF-A Direct Request 2 and a compatible 64-bit firmware partition
(layout in [Transport](#transport-and-attribution)).

### Tested

Native ARM64 build, module load, packaging, and supervised EC exchanges on
PGX and DGX Spark (NVIDIA kernels including `6.11.0-1016` and `6.17.0-1032`).
Isolated DKMS builds with DKMS 3.0.11 and 3.4.1.

Checked: floor request, automatic reset, cap rejection, locking, SIGINT
cleanup, missing-sensor maximum-floor path, NVML and thermal-zone reads, idle
floor, `--curve`, `max` @ 9000 with `fan_fault: 0`, governor band changes.

**Not** checked: tachometer truth, sustained-load cooling at the 9000 top band,
package-install fault injection, other SKUs/firmware. A passing ack is not a
cooling validation.

## Migrate from upstream

Same module name (`nvfancontrol`). The Debian package conflicts so both cannot
be installed. Do not run old and new controllers together. Installing a new
`.ko` does not replace a loaded module.

On a clean machine, skip this section.

1. Find old units (`systemctl list-unit-files`), the upstream package
   (`dpkg-query -W nvfancontrol`), and `lsmod`. Stop only units that exist:

   ```sh
   sudo systemctl disable --now nvfancontrol.service
   sudo systemctl disable --now gb10-fanctl-governor.service
   ```

2. With the **old** module still loaded, restore automatic control using **its**
   tool (`sudo nvfancontrol auto` or `sudo gb10-fan auto`) and confirm status.
   An older extended controller must show **both** cap and floor automatic.
   Stop any new governor first. **Do not unload** if restore fails or transport
   health is uncertain.

3. Only then: `sudo modprobe -r nvfancontrol`. Remove the upstream package
   only if it is installed. Clean known leftover DKMS entries with that
   project's uninstall steps. Do not delete unknown DKMS entries.

These are healthy-migration steps, not fault recovery.

Package upgrades stop both units, do not restart them, and do not replace a
loaded module. Restart only after you deliberately load the new driver.
Enablement links are preserved on upgrade and removed on purge. No old shell
configuration is overwritten.

## Failures and retained state

Transport errors, timeouts, reply mismatch, or failed shared-buffer restore
latch `fan_fault` and block further driver requests. The executable stops after
a write failure. It does not blindly reset or replay.

Only a mailbox-busy condition **before submission**, with no latched fault, is
retried (up to three times).

`SERVICE FAILURE` kernel logs include firmware service status, FF-A duration,
request bytes, and the first four response words. Upstream associates status
`5` with a failed status read, request write, or doorbell write. A call
≥ 40 ms is flagged as a **possible** eSPI completion timeout — classification
only, not a tunable. That is distinct from `OUTPUT TIMEOUT` (5 s wait for the
reply-ready flag after a zero service status). Stretching that wait cannot fix
a status-`5` failure; the driver never reaches it.

SIGKILL, crashes, stop timeouts, and module unload can leave EC overrides in
effect. Load/unbind issues no reset; cached state starts unknown. Unload is
not a power cycle. If you cannot establish cooling, stop the workload and use
the platform's supported shutdown procedure.

## Transport and attribution

Upstream reverse engineering identifies FF-A partition UUID
`884a63a0-3285-4120-83aa-eec008a0a546` (observed as `arm-ffa-17`, ID `0x11`,
properties `0x109`, 64-bit mode), Direct Request 2, eSPI OEM command **17**,
and manifest `ns_shm0` at physical `0x933dd000`, size `0x1000`. Verify
firmware/kernel compatibility. Do not edit the UUID or address to force a match.

The 32-byte mailbox frame carries a five-byte
`07 <inner> 00 <target LE16>` request at offset `0x10`. Inner `3` writes cap
slot `0x119190`, inner `5` writes floor slot `0x119192`, `0xffff` disables a
slot.

The driver requires an idle mailbox, rejects unreserved Linux RAM mappings,
validates service status and the exact reply, polls up to 5 s at 10 ms, and
restores/verifies the initial 32 bytes only after a successful exchange. That
is not exclusive ownership against other mailbox clients.

The driver assumes RPM mode. It does not query the EC mode or read override
slots back. Upstream
[protocol notes for EC firmware 0x3000508](https://github.com/Z841973620/dgx-spark-fan-override/blob/7ffcb3e28327d3e0f4d62210845d3357f0fbe256/README_EN.md)
also describe a percentage mode, mode/range query (inner `1`), slot readback
(`2`, `4`), and telemetry (`7`). Those are not implemented here.

Derived from [Z841973620/dgx-spark-fan-override](https://github.com/Z841973620/dgx-spark-fan-override)
by **841973620**. **Modified 2026-09-05:** dual-slot controls, transport fault
handling, single C CLI/governor, packaging. Licensed **GPL-2.0-only**, without
warranty. [LICENSE](LICENSE) reproduces the upstream license verbatim.

The upstream base revision is **unverified**. The original import did not pin a
commit SHA. `kernel/nvfancontrol.c` is a vendored, modified copy.