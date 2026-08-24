# Capturing the vendor driver's audio-enable sequence (Windows)

> **v2 -- CRITICAL FIX after the first trial.** The first capture was taken while
> the Elgato app was already streaming, so it recorded only steady-state polling
> and missed the audio-enable entirely. The capture MUST be running BEFORE the
> app opens the stream. The one change that matters: **close the Elgato app
> first, start the Wireshark capture, and only THEN open the app.**

Goal: record the USB control traffic the Elgato Windows driver sends while audio
is actually working, so we can diff it against our Linux bring-up and find the
one thing that turns audio on. This is a read-only capture — it changes nothing
on the device.

## What you need
- The HD60 S, a USB 3 cable, and an HDMI source that is **playing audio**
  (the Switch is fine — have a game or menu making sound).
- The Elgato capture app (4K Capture Utility or Game Capture HD), OR OBS with the
  HD60 S added as a source. Anything that shows the picture AND moving audio meters.
- Wireshark. **During install, tick the "USBPcap" component** (it is off by
  default). Reboot afterwards — USBPcap needs it.

## Phase 1 — confirm audio really works first
1. Plug the HDMI source into the HD60 S **IN**, HD60 S into the PC via USB 3.
2. Open the capture app, start the preview.
3. Confirm you see the video AND the audio meters moving / hear sound.
   If audio does not work here, stop — the capture would be useless. Tell me.
4. Close the app completely.

## Phase 2 — pick the right capture interface
1. Launch Wireshark **as Administrator** (right-click → Run as administrator).
2. In the interface list you'll see entries named **USBPcap1, USBPcap2, …**
   (one per USB root hub).
3. Click the small arrow / expand each USBPcapN to see the device tree, and find
   the one that lists **"Elgato"** or **"Game Capture HD60 S"**. Note its number.
   - If you can't tell, it's fine to select **all** USBPcapN interfaces at once
     (Ctrl-click each). The file is just bigger.

## Phase 3 — set a snap length (keeps the file small, prevents drops)
The video stream is ~250 MB/s; we do NOT want that data, only the small control
messages. Truncating each packet keeps the file tiny and stops Wireshark dropping
packets.
1. **Capture → Options**.
2. Find your USBPcap interface row. In the **"Snaplen (B)"** column, double-click
   and set it to **512**. (Do this for each USBPcap interface you selected.)
3. Leave everything else default. Don't start yet.

## Phase 4 — the capture
1. In Capture → Options, click **Start**. Capture is now running.
2. **Unplug and replug the HD60 S USB cable.** (This forces a full cold init so
   the whole enable sequence is recorded.)
3. Wait ~5 seconds, then open the capture app and start the preview.
4. Once you can confirm audio is flowing (meters moving), let it run **15 seconds**.
5. Stop the capture (red square).

## Phase 5 — sanity-check before sending
1. In the display-filter bar at the top, paste this and press Enter:
   ```
   usb.bmRequestType == 0x40 || usb.bmRequestType == 0xc0
   ```
2. You should see a list of rows (these are the vendor control commands — the
   gold). If the list is **not empty**, the capture is good.
   - If it's empty, you captured the wrong root hub. Redo Phase 2 selecting all
     USBPcapN interfaces.
3. **File → Save As** → name it `hd60s-audio.pcapng`.

## Phase 6 — get the file to the Linux machine
Copy `hd60s-audio.pcapng` over (USB stick, network share, cloud — whatever's
easy) and drop it in this folder:
`~/Desktop/ElGato Linux Driver/docs/captures/`
Then tell me it's there and I'll take it from there.

## Optional but helpful
If you can, also do a second short capture the **same way but with audio muted /
no audio source**, saved as `hd60s-noaudio.pcapng`. The difference between the two
is the smoking gun. Not required — the single audio capture is usually enough.
