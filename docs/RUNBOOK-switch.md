# Runbook - mapping the HD60 S status protocol with a Nintendo Switch

Goal: find which bytes the device uses to report *HDMI signal present*,
*resolution*, and *HDCP state*, without knowing a single vendor command yet.

The trick is **differential capture**. We don't need to understand the protocol
to make progress - we only need two captures that differ in exactly one
physical variable. Whatever bytes change are, by definition, the bytes that
encode that variable.

Everything here is read-only. Safe to run repeatedly.

---

## Before you start

**Hardware**
- Switch must be **docked** - handheld mode has no HDMI output.
- Dock's **HDMI OUT** -> HD60 S **HDMI IN**. The HD60 S also has an HDMI OUT
  (passthrough to a TV); it is the wrong port for this.
- A TV on the passthrough is optional but handy: it confirms the Switch is
  actually outputting.

**HDCP** - Switch *games* are HDCP-free and will capture. The eShop and video
streaming apps assert HDCP and will not. Sit on a game or the HOME menu.

**Sanity check first**

```bash
cd "/home/sean/Desktop/ElGato Linux Driver"
./tools/hd60s-linkcheck.py
mkdir -p docs/captures
```

Expect 480M for now. That is fine - every step below works at 480M, because
we are reading the status channel, not video.

---

## Step A - baseline, nothing connected

Unplug HDMI from the HD60 S input entirely. Leave the USB cable in.

```bash
./hd60s-probe --seconds 20 2>&1 | tee docs/captures/A-idle-noinput.log
```

Read the last line, `captured N interrupt packet(s)`:
- **N = 0** -> the endpoint is demand-driven, silent until asked or until
  something happens. Expected.
- **N > 0** -> it free-runs. Note the interval between packets.

## Step B - vendor request sweep, nothing connected

```bash
./hd60s-probe --sweep --seconds 0 2>&1 | tee docs/captures/B-sweep-noinput.log
```

Takes a minute or two - it tries 256 request codes at two recipients. Most
stall, which is normal and means "not implemented". The tail prints how many
responded.

## Step C - connect the Switch, then listen

Now connect: Switch docked and **powered on with a game or HOME menu showing**,
dock HDMI OUT -> HD60 S HDMI IN.

```bash
./hd60s-probe --seconds 20 2>&1 | tee docs/captures/C-idle-switch1080.log
```

## Step D - vendor request sweep, Switch connected

The important one.

```bash
./hd60s-probe --sweep --seconds 0 2>&1 | tee docs/captures/D-sweep-switch1080.log
```

## Step E - the diff (this is the payoff)

```bash
diff -u docs/captures/B-sweep-noinput.log docs/captures/D-sweep-switch1080.log
```

Any `bRequest` whose returned bytes differ between B and D is a **status
register that reflects HDMI input state**. That is the first confirmed piece of
the register map, and it comes without a single guessed constant.

Record each one in `docs/PROTOCOL.md` as `[OBSERVED]`.

## Step F - hotplug transition

Start this, then **unplug the HDMI cable at roughly 10 seconds** and plug it
back in around 25 seconds.

```bash
./hd60s-probe --seconds 40 2>&1 | tee docs/captures/F-hotplug.log
```

If packets arrive on EP 0x81 within a moment of you unplugging or replugging,
the device pushes asynchronous events and the daemon needs an event loop rather
than polling. If nothing arrives even across a hotplug, EP 0x81 is purely
demand-driven - also a useful, design-shaping result.

## Step G - resolution change

On the Switch: **System Settings -> TV Output -> TV Resolution**. Capture at two
resolutions.

```bash
# set Switch to 1080p first
./hd60s-probe --sweep --seconds 0 2>&1 | tee docs/captures/G-sweep-1080p.log
# now switch it to 720p, then:
./hd60s-probe --sweep --seconds 0 2>&1 | tee docs/captures/G-sweep-720p.log

diff -u docs/captures/G-sweep-1080p.log docs/captures/G-sweep-720p.log
```

Bytes that change here encode **resolution/timing**. Combined with step E you
can usually tell a signal-present flag apart from a format field: the
signal-present bit changes in E but not in G, while format fields change in G.

---

## What to send back

The diffs from steps E and G, plus the packet counts from A, C and F. Those
four facts are enough to start naming registers.

Keep the raw `.log` files. Byte-level detail matters later in ways that a
summary destroys.

## What this cannot do

None of this reveals the **command set that starts video**. Read-only probing
finds status registers, not the write sequence that configures and starts a
stream. That needs a `usbmon` trace of the official Windows/macOS driver.
See `docs/EXPERIMENTS.md` E5.
