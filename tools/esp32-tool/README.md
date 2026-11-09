# esp32-tool

Work with an ESP32 over USB serial from a host that has **no IDF toolchain** — flash it, watch it,
drive its CLI, and pull its diagnostics off it.

Built for the split workflow: build on a fast toolchain host, then flash and watch on the host the
boards are actually plugged into. That host needs only python3 + pyserial, and esptool for
flashing — not the compiler, cmake or ninja.

```
esp32-tool flash    -p PORT -b BUILD_DIR [--pull HOST:PATH]   # write image to chip
esp32-tool monitor  -p PORT [command]                          # print console (interactive)
esp32-tool command  -p PORT <cmd>                              # one command, just its answer
esp32-tool reset    -p PORT                                    # pulse the reset line and exit
esp32-tool diag     extract|decode|clear                       # blackbox diagnostics partition
esp32-tool deploy   -p PORT -b BUILD_DIR [--pull HOST:PATH]    # flash, then monitor
```

`flash` and `deploy` also take **`-f NAME-VERSION`**, which flashes a *staged release* out of the
staged release tree instead of a build directory — the manifest beside it supplies the chip, the
offsets and the binaries, so one name covers the whole set:

```sh
esp32-tool deploy -b /opt/iotdata/src/iotdata-deploy/se-67194/release/esp32c3/bin -f relay-0.9.9
```

Staging is not this tool's job (see `iotdata-platform/tools/ota-stage`); flashing what was staged is.


Everything the flash needs — chip, flash settings, the three binaries and their offsets — is read
from the build dir's `flasher_args.json`, so it works for any project without hardcoding anything.
`monitor` is the same coloured-log / reconnect / command-inject loop as
`iotdata-device/sds/tools/esp32-boot`; this tool is a superset.

## Typical two-host flow

Build on the toolchain host:

```sh
# on workshop
cd iotdata-example/simulator_sensor_lora_esp32 && make
```

Flash and watch on the host the boards live on (e.g. `iotdata-tst-3`):

```sh
# artifacts already local (shared FS / copied):
esp32-tool deploy -p /dev/ttyACM0 -b ../simulator_sensor_lora_esp32/build

# or pull the binaries straight off the build host, then flash + monitor:
esp32-tool deploy -p /dev/ttyACM1 \
    --pull workshop:/opt/iotdata/src/iotdata-example/simulator_sensor_lora_esp32/build
```

## Boards that sleep

A board that deep-sleeps between cycles is only enumerated for the few seconds it is awake, so
`/dev/ttyACM0` mostly does not exist and the flash window is easy to miss — losing it costs a whole
sleep interval. **`-w` waits for the port** instead of failing, on every subcommand that uses one:

```sh
esp32-tool deploy -p /dev/ttyACM0 -w      # sit until it wakes, flash it, then monitor
```

The build dir is resolved (and `--pull` done) *before* the wait, so the window is spent flashing
rather than on setup. `monitor` already survives the re-enumeration on its own.

## Driving the device CLI

`command` issues one command and prints only its response — the C-tagged lines — with log noise
filtered out, which is what makes it scriptable:

```sh
esp32-tool command -p /dev/ttyACM0 vers
esp32-tool command -p /dev/ttyACM0 "diag flush"
```

## Diagnostics

`diag` pulls the blackbox diagnostics partition off the chip with esptool and decodes it to CSV —
far faster than streaming the records over the console, and it works on a device that is not
talking. It implements the same record walk as the blackbox decoder rather than importing it, so
the two stay independent (see `iotdata-depend/blackbox/README.md`).

```sh
esp32-tool diag extract -p /dev/ttyACM0 --raw diag.bin --out diag.csv
esp32-tool diag decode diag.bin          # offline, re-decode an image pulled earlier
esp32-tool diag clear --confirm          # erase it
```

NB `diag` and `flash` reset the device; `monitor` does not unless you pass `-r`.

## Resetting, and watching, a board with no console

`reset` pulses the line and exits — RTS drives EN, DTR drives GPIO0, the same auto-reset the
flashing tools use, so it needs no toolchain and no cooperation from the firmware. That matters for
a node whose application has wedged, and more for one with **no console at all**: a sensor cannot be
told to forget anything, so a pulse is the only way back to a known state short of reaching for the
cable. Being a subcommand it takes `-s MAC` like the rest, which is the point — `ttyACM`
numbering moves across exactly the reset you are performing.

Redirected to a file, `monitor` is a **recorder**: no colour, and every line flushed as it arrives
rather than when a buffer fills. It prints `--- DEVICE CONNECTED <port>` when it attaches, so a
script can tell "started and attached" from "started and died on a busy port" — and it keeps
monitoring when its stdin is `/dev/null`. The system tests in `iotdata-testing` use it exactly this
way for the sensors, which have no other voice:

```sh
esp32-tool monitor -p /dev/ttyACM0 >> sensor1.log 2>&1 &   # the only observation there is
esp32-tool reset   -p /dev/ttyACM0                          # back to an empty table
```

## Installing

The tool is one self-contained python file, so a host with boards plugged into it needs neither
this repo nor a toolchain — just the script and two python packages from its own distro.

```sh
make check                      # what is present here, and how to fix what is not
make deps                       # install the missing python packages
make install                    # -> /usr/local/bin/esp32-tool   (PREFIX=, BINDIR=, DESTDIR= honoured)

make install-remote HOST=pi@box # copy it to a board host and check ITS dependencies
```

`make deps` prefers the distro package (`python3-serial`, `esptool`) over pip, because Debian marks
its python externally-managed (PEP 668) and a plain `pip install` there either refuses or starts
quietly fighting apt. It falls back to `pip --user`, and to `--break-system-packages` only after
saying that is what it is doing.

## Dependencies on the target host

None of these is the compiler toolchain, and the tool is useful before they are all present —
`make check` reports them per feature rather than pass/fail:

| For | Needs | Install |
|---|---|---|
| `list`, and everything | python3 | the only hard requirement |
| `monitor`, `command`, `reset` | python3, pyserial | `python3-serial`, or `pip install pyserial` |
| `flash`, `diag` | esptool | `esptool` (apt), or `pip install esptool` |
| `--pull` | rsync + ssh | usually already present |

Note that an ESP-IDF installation carries its own esptool inside its venv, which is **not** on the
system python's path — so a machine that builds and flashes happily with `idf.py` can still report
esptool missing here. They are separate installs.

## Notes

- **Two boards / stable ports.** `ttyACM` numbering can move across a reset or re-enumeration. To
  pin a physical board, address it by a stable path: `-p /dev/serial/by-path/<...>` (see
  `ls -l /dev/serial/by-path`).
- **Port default** is `$ESP32_PORT`, then `/dev/ttyACM0`.
- Exit the monitor with **Ctrl-C**.
