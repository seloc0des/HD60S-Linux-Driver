# HD60 S bring-up plan

Reconstructed from Elgato's Windows driver (`CY3014.X64.SYS`) by five
independent disassembly traces, each adversarially verified, then merged.
Every step is tagged CONFIRMED (the disassembly clearly shows it) or
SPECULATIVE (inferred).

`scripts/vendor-bringup.hd60s` implements this. Running it on real hardware
executes cleanly -- every register write takes -- but produces no data on
endpoint 0x83. See the gap analysis at the end.

## Steps

### 1. usb_setup  *(SPECULATIVE)*

Open USB 0fd9:005e. SET_CONFIGURATION 1. Claim interface 0, leave alternate setting 0. Do NOT upload firmware — GameCaptureHD60S.inf ships no .hex and no CopyFiles entry references one; the device runs resident firmware and enumerates directly as 0fd9:005e.

> INF grep: only CopyFiles = GameCapture.CopySys/CopyDll32/CopyDll64. Descriptor layout itself was never read — no descriptor dump exists in this package.

### 2. vendor_out  *(CONFIRMED)*

bmRequestType 0x40, bRequest 0xEC, wValue 0x0000, wIndex 0x0000, 10 bytes: B8 22 00 00 00 C0 00 00 00 CA

> 0x6fc58-0x6fca5. Only offsets 0,1,5,7,9 are written (0xB8,0x22,0xC0,0x00,0xCA at 0x6fc78/0x6fc80/0x6fc95/0x6fc9d/0x6fc8d); offsets 2,3,4,6,8 are uninitialised stack at [rsp+0x80] so Windows sends garbage there — send zeros. Reached for 0x5e via 0x6fbd2 'cmp WORD [rcx+0xa],0x5e' / je 0x6fc58.

### 3. vendor_out  *(CONFIRMED)*

bmRequestType 0x40, bRequest 0xC2, wValue 0x0000, wIndex 0x0000, ZERO LENGTH. Ignore the result.

> 0x6fd26-0x6fd47. Unconditional join point of the PID chain; eax is never tested afterwards (next insn 0x6fd4c is 'mov rsi,[rbx+0x1c788]'). Semantics unknown — see unknowns.

### 4. vendor_out  *(CONFIRMED)*

bmRequestType 0x40, bRequest 0xC7, wValue 0x0064, wIndex 0x0000, ZERO LENGTH. Success is optional; on success the driver sets a capability bit.

> 0x6fd53-0x6fd6e, then 'or BYTE PTR [rsi+0x17d1],0x8' at 0x6fd77.

### 5. fpga_write  *(CONFIRMED)*

reg 0x39 = 0xC0  ->  bRequest 0xC1, wValue 0xC039, wIndex 0, no data stage

> 0x19ff1 inside CheckIsMCUExist (0x19f80). This is the non-04bb:0543 arm; the 04bb arm at 0x19fd6 uses reg 0x33 instead and does not apply.

### 6. fpga_write  *(CONFIRMED)*

reg 0x34 = 0x41  ->  bRequest 0xC1, wValue 0x4134, wIndex 0, no data stage. This releases/starts the MCU.

> 0x19ffd. NOTE the EnableMCU helper at 0x1a590 uses 0x40 not 0x41 for this register (tail-jump at 0x1a5cf) — use 0x41 here, it is the bring-up value.

### 7. poll  *(CONFIRMED)*

Repeat up to 100 times: delay 10 ms, then fpga_read(0x39); stop when the returned byte == 0x01. Note the delay comes FIRST, before the first read.

> Loop head 0x1a010 'mov ecx,0xa / call 0x7ac60', read at 0x1a052, 'cmp al,0x1 / je 0x1a062' at 0x1a055, bound 'cmp ebx,0x64 / jl 0x1a010' at 0x1a05d.

### 8. mcu_write+mcu_read  *(CONFIRMED)*

MCU-alive probe. mcu_write(0x66, [AB 03 12 34 57]) i.e. bRequestType 0x40 / bRequest 0xC0 / wValue 0x5066 / 5 bytes OUT; then mcu_read(0x66, 3) i.e. bRequestType 0xC0 / wValue 0x5066 / 3 bytes IN. SUCCESS CRITERION: reply[0]==0x51 AND reply[1]==0x10. On any other reply, delay 100 ms and retry, up to 200 attempts. A reply of 33 44 55 specifically means 'busy, retry'.

> Payload bytes 0x12/0x34/0x57 at 0x1a096/0x1a09b/0x1a0ae, 0xAB and count 3 at 0x1a136/0x1a13f, length 5 via 'lea r9d,[rbx+0x5]' at 0x1a0db, write 0x1a144, read 0x1a16f. Gate: 0x1a2d3 'cmp bpl,0x51' / 0x1a2d9 'cmp sil,0x10' / je 0x1a318 -> 'mov BYTE PTR [rdi+0x63f0],0x1'. 33/44/55 retry at 0x1a2fb-0x1a316. Retry bound r12d=0xc8 at 0x1a0a3.

### 9. mcu_write+mcu_read  *(CONFIRMED)*

OPTIONAL firmware-date read. delay 100 ms, then mcu_write(0x66, [AB 03 12 34 58]); mcu_read(0x66, 3). Sanity range: b0 in 0x14..0x28, b1 in 0x06..0x0C, b2 in 0x01..0x1F (yy/mm/dd). Failure is non-fatal on this device.

> 0x1a332-0x1a33c set 12 34 58; delay 0x1a341; read 0x1a3fa; range checks 0x1a496-0x1a4ac; on failure 'cmp DWORD [r13+0x17fc],0' at 0x1a4ae -> je 0x1a4e1 stores anyway (our b_is_ite6802_support is 0).

### 10. i2c_write+i2c_read  *(CONFIRMED)*

MST3367 presence probe: mst_write(page 0, reg 0x07, 0xA5) then mst_read(page 0, reg 0x07). Reading back 0xA5 proves the MCU->I2C->MST3367 chain works end to end. This is the single best go/no-go checkpoint in the whole plan.

> 0x1a551-0x1a569, tail of CheckIsMCUExist: 'mov r9b,0xa5 / mov r8b,0x7 / xor edx,edx / call 0x65080' then 'mov r8b,0x7 / xor edx,edx / call 0x65230'.

### 11. vendor_out  *(CONFIRMED)*

bmRequestType 0x40, bRequest 0xC6, wValue 0x0000, wIndex 0x0100 (= mcu_present<<8, and mcu_present is now 1), ZERO LENGTH. Do NOT send bRequest 0xC5 — it is deliberately skipped for this PID.

> 0xC6 at 0x6ff14-0x6ff37; skip of 0xC5 at 0x6fe4b 'cmp WORD [rcx+0xa],0x5e' / 0x6fe50 'je 0x6ff14' which lands past the 0xC5 block at 0x6fee1.

### 12. cpld_read  *(CONFIRMED)*

cpld_read(0x3B) -> v. If v == 0x00, SKIP step 13 entirely.

> CY3014_HwReset (0x6b0a0) entered at 0x6b2bc for PID 0x5e (0x6b124 'cmp WORD [rcx+0xa],0x5e' / 0x6b129 'je 0x6b2bc'). Register index 0x3b staged at 0x6b2c8; skip test 0x6b342 'cmp BYTE [rsp+0x40],sil' / 0x6b347 'je 0x6b4f5'.

### 13. cpld_write  *(CONFIRMED)*

cpld_write(0x3B, 0x02). The 0x02 is the HDMI-route value; 0x80 is the analog-route value and does NOT apply here.

> Route branch 0x6b34d/0x6b43f on [dev+0x7104]; our route is 0 (INF line 189 sets AnalogCrossbarVideoInputProperty=0, code default also 0), so 0x6b445 'test eax,eax / je 0x6b452' -> 0x6b459 'mov BYTE PTR [rsp+0x40],0x2', written at 0x6b4ba.

### 14. cpld_write  *(CONFIRMED)*

cpld_write(0x20, 0x05). Unconditional.

> 0x6b4fc 'mov BYTE PTR [rsp+0x40],0x5' with the register index 0x20, sent at 0x6b55d.

### 15. cpld_write  *(CONFIRMED)*

cpld_write(0x00, 0x01)  — GPIO data port 0, pin 0 driven high.

> 0x6b5fb 'mov BYTE PTR [rsp+0x40],0x1' with r9w=0, sent at 0x6b65a.

### 16. cpld_write  *(CONFIRMED)*

cpld_write(0x10, 0xFE)  — GPIO direction port 0: pin 0 = output, pins 1-7 = inputs (0 = output).

> 0x6b5a7 'mov dil,0xfe'; 0x6b6c9 stages reg 0x10 and value dil; sent at 0x6b6ce. The 0xFC alternative at 0x6b5c7 applies only to PIDs 0x531/0x532.

### 17. note  *(CONFIRMED)*

DO NOT replay the FX3 bRequest-0xC1 register-0x2D reset block (wValue 0x402D, 0x012D, 0x002D, 0x012D, 0x802D) that sits at the head of CY3014_HwReset, and do not replay CPLD data port 1 / direction port 1. Both are jumped over for PID 0x005E.

> 0x6b129 'je 0x6b2bc' skips 0x6b1ca-0x6b2b7 (the 0x2D block plus a page0 0x51=0x80 write). Separately 0x426a8 'je 0x4276e' skips the CPLD reg 0x01 / reg 0x11=0xFC block at 0x42733-0x42769, and 0x18b0c 'je 0x18bf5' skips the same in TMDS_HOT_PLUG.

### 18. i2c_write  *(CONFIRMED)*

mst_write(0, 0x13, 0x08)

> Start of MST3367_HwInitialize's own register work. Function entry is 0x425d0 (NOT the PDB's 0x427b0, which is mid-instruction). Write staged 0x425f8-0x42600, call at 0x42661.

### 19. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(0, 0xB7) ; mst_write(0, 0xB7, v | 0x02)

> 0x42776 read, 0x42780 'or al,0x2', 0x42789 write.

### 20. i2c_write  *(CONFIRMED)*

mst_write(0, 0x41, 0x6F) ; mst_write(0, 0xB8, 0x00)

> 0x4278e-0x42799 and 0x4279e-0x427a9.

### 21. i2c_write  *(CONFIRMED)*

mst_write(1, 0x0F, 0x02) ; mst_write(1, 0x16, 0x30)

> 0x427ae-0x427b9 and 0x427be-0x427c9.

### 22. i2c_write  *(CONFIRMED)*

mst_write(1, 0x17, 0x00) ; mst_write(1, 0x18, 0x00) ; mst_write(1, 0x19, 0x00)

> Reached NOT via the 0x427ce guard (that field is CustomCompanyAlconProperty = 0, so 'jne 0x4280c' is taken) but via the [dev+0x78f0] = CustomAnalogVideoInputEqProperty cascade: default 1, so 0x4280c 'cmp ...,0' jne -> 0x42845 'cmp ...,1' equal -> writes 0x00 at 0x42859/0x42869/0x42879 -> 0x4287e 'cmp ...,2' jb 0x428bd. Same bytes either way. I disassembled 0x427c9-0x428d7 to confirm.

### 23. i2c_write  *(CONFIRMED)*

mst_write(1, 0x1A, 0x50)

> 0x428bd-0x428d2.

### 24. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(1, 0x2A) ; mst_write(1, 0x2A, v | 0x07)

> 0x428df read, 0x428f2 write.

### 25. i2c_write  *(CONFIRMED)*

mst_write(2, 0x08, 0x03)

> 0x42902.

### 26. i2c_write+delay+i2c_read  *(CONFIRMED)*

RUN ONCE PER PROCESS LIFETIME ONLY. mst_write(1,0x25,0x00) ; mst_write(1,0x26,0x00) ; then 5 iterations of { delay 20 ms ; b = mst_read(1,0x27) } keeping the 5 bytes. Skip this whole step on any later re-init.

> Gated by a driver-global one-shot byte at .data 0x3ada91: 0x42a4c 'cmp BYTE PTR [rip+0x36b03e],0' / 0x42a53 'jne 0x42aa8', latched at 0x42a60. Writes 0x42a67/0x42a77; loop 0x42a86-0x42aa6 with 'mov ecx,0x14' (20 ms) and 5 iterations, bytes stored to global 0x3ada8c. Those 5 bytes are replayed later at 0x6ee8c-0x6eeac.

### 27. i2c_write  *(CONFIRMED)*

mst_write(1, 0x24, 0x40)

> 0x42aa8 'mov eax,[rdi+0x5ecc]' / 'test eax,eax' / 'je 0x42ac4' -> 0x42ac4 'mov r9b,0x40', call 0x42acf. [dev+0x5ecc] is the analog-block open refcount, zeroed at 0x23bac and only incremented at 0x6e1eb/0x6e1fd, so it is 0 on first bring-up. Value 0x41 applies only after an analog open.

### 28. i2c_read  *(CONFIRMED)*

v = mst_read(1, 0x24). If (v & 0x01) != 0 then additionally: mst_write(1,0x25,0x00) ; mst_write(1,0x26,0x00) ; mst_write(1,0x27,0x00) five times. Expected outcome with 0x40 just written: bit0 clear, whole block skipped.

> 0x42b09 read, 0x42b18 'test al,0x1' / 0x42b1a 'je 0x42b54'; clear loop 0x42b1c-0x42b52 ('dec esi / jne 0x42b40', esi=5 from 0x4290e). Outcome is decided by silicon, not code — hence the conditional.

### 29. i2c_write  *(CONFIRMED)*

mst_write(1,0x30,0x80) ; mst_write(1,0x31,0x00) ; mst_write(1,0x32,0x00)

> 0x42b5f, 0x42b6f, 0x42b7f.

### 30. i2c_write  *(CONFIRMED)*

mst_write(0, 0xB0, 0x14)

> 0x42b8f.

### 31. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(0, 0xAE) ; mst_write(0, 0xAE, v | 0x04)

> 0x42b9c read, 0x42baf write.

### 32. i2c_write  *(CONFIRMED)*

mst_write(0, 0xAD, 0x05)

> 0x42bb4 'cmp DWORD PTR [rdi+0x7910],0' / 0x42bc0 'jbe 0x42bd4'; cmp-against-0 always clears CF so jbe fires exactly on equality. [dev+0x7910] = registry CustomAnalogVideoNativeColorSpaceProperty (name at 0x296b00), fallback r15d=0, absent from the INF -> field is 0 -> 0x05 at 0x42bd4/0x42bdf.

### 33. i2c_write  *(CONFIRMED)*

mst_write(0,0xB1,0xC0) ; mst_write(0,0xB2,0x00) ; mst_write(0,0xB3,0x00)

> 0x42bef, 0x42bff, and 0xB3 at 0x42c2b/0x42c36. The 0xB3=0x14 alternative at 0x42c19 needs USB descriptor iProduct==0x05 AND iManufacturer==0x5f ([dev+0x120]+0xf / +0xe); see unknowns — this is the one input I could not observe.

### 34. i2c_write+i2c_read+i2c_write  *(CONFIRMED)*

mst_write(0,0xB4,0x55) ; v = mst_read(0,0xB4) ; mst_write(0,0xB4, v & 0xFC)   (in practice 0x55 -> 0x54)

> 0x42c46 write, 0x42c53 read, 0x42c5d 'and al,0xfc', 0x42c66 write. I disassembled this block directly.

### 35. i2c_write  *(CONFIRMED)*

mst_write(2,0x01,0x61) ; mst_write(2,0x02,0xF5)

> 0x42c76, 0x42c88.

### 36. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x03) ; mst_write(2,0x03, v | 0x02)

> 0x42c95 read, 0x42c9f 'or al,0x2', 0x42ca8 write.

### 37. i2c_write  *(CONFIRMED)*

mst_write(2,0x04,0x01) ; mst_write(2,0x05,0x00) ; mst_write(2,0x06,0x08)

> 0x42cb8, 0x42cc8, 0x42cd8.

### 38. i2c_write  *(CONFIRMED)*

mst_write(2,0x1C,0x1A) ; mst_write(2,0x1D,0x00) ; mst_write(2,0x1E,0x00) ; mst_write(2,0x1F,0x00)

> 0x42ce8, 0x42cf8, 0x42d08, 0x42d18.

### 39. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x25) ; mst_write(2,0x25, v | 0xA2)

> 0x42d25 read, 0x42d2f 'or al,0xa2', 0x42d38 write.

### 40. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x02) ; mst_write(2,0x02, v | 0x80)   (0xF5 from step 35 already has bit7 set, so this is usually a no-change write)

> 0x42d47 read, 0x42d52 'or al,0x80', 0x42d5c write.

### 41. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x07) ; mst_write(2,0x07, v | 0x04)

> 0x42d69 read, 0x42d73 'or al,0x4', 0x42d7c write.

### 42. i2c_write  *(CONFIRMED)*

mst_write(2,0x17,0xC0) ; mst_write(2,0x19,0xFF) ; mst_write(2,0x1A,0xFF) ; mst_write(2,0x1B,0xFC) ; mst_write(2,0x20,0x00)

> 0x42d8c, 0x42d9c, 0x42dac, 0x42dbc, 0x42dcc — all verified in my own disassembly of 0x42d7c-0x42dcc.

### 43. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x21) ; mst_write(2,0x21, v & 0xFC) ; then mst_write(2,0x22,0x26)

> 0x42dd9 read, 0x42de3 'and al,0xfc', 0x42dec write; 0x42dfc for reg 0x22.

### 44. i2c_write  *(CONFIRMED)*

mst_write(2, 0x27, 0x00)

> 0x42e01 'cmp DWORD PTR [rdi+0x7118],1' / 0x42e08 'jb 0x42e1c' -> 0x00 at 0x42e27. [dev+0x7118] = AnalogCrossbarAudioInputProperty; its CODE default is 3 (0x2503d 'mov edx,0x3') which would give 0xFF at 0x42e15, but GameCaptureHD60S.inf line 188 sets it to REG_DWORD 0 — I grepped the INF and confirmed. 0x00 is what a stock Windows install programs.

### 45. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(2,0x2E) ; mst_write(2,0x2E, v | 0xA1)

> 0x42e34 read, 0x42e3e 'or al,0xa1', 0x42e47 write.

### 46. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(0,0xAB) ; mst_write(0,0xAB, (v & 0x95) | 0x15)   then   v = mst_read(0,0xAC) ; mst_write(0,0xAC, (v & 0xD5) | 0x15)

> 0x42e54/0x42e5e/0x42e63/0x42e69 and 0x42e76/0x42e80/0x42e85/0x42e8b. Note the two masks differ (0x95 vs 0xD5) — do not copy-paste one for the other.

### 47. i2c_read+i2c_write  *(CONFIRMED)*

v = mst_read(0,0xB7) ; mst_write(0,0xB7, v | 0x02)

> Reached because [dev+0x7104]==0: 0x42e90 'mov eax,[rdi+0x7104]' / 0x42e96 'test eax,eax' / 0x42e98 'je 0x42ea3', then the VID/PID chain 0x42ee0 'cmp WORD [rcx+0xa],0x5e' / 0x42ee5 'je 0x42fab' (which also skips the CPLD block at 0x42f68). Read 0x42fb3, 'or al,0x2', write 0x42fc6.

### 48. i2c_write  *(CONFIRMED)*

mst_write(0,0xB8,0x10) then immediately mst_write(0,0xB8,0x00) — a reset pulse, NOT a branch. Then mst_write(2,0x07,0xF4) then mst_write(2,0x07,0x04) — also a pulse, also not a branch.

> 0x42fcb/0x42fdb and 0x42feb/0x42ffb are straight-line. I re-checked by enumerating all 129 branch targets in the function: inside 0x42fab..0x43027 the only target is 0x42fab itself.

### 49. i2c_write  *(CONFIRMED)*

mst_write(0, 0x51, 0x89)

> 0x43275-0x43280, reached from the 4th VID/PID chain: 0x431f6 'cmp WORD PTR [r11+0xa],0x5e' / 0x431fc 'je 0x43275'. No 0xfff masking on the PID compare (siblings do mask; this one does not).

### 50. i2c_read+i2c_write  *(CONFIRMED)*

MST3367_TMDS_HOT_PLUG(3): v = mst_read(0,0xB7) ; mst_write(0,0xB7, v & ~0x02). This DE-asserts, because flag bit1 is set.

> 0x43285 'mov edx,0x3' / 0x4328d 'call 0x18ab0'. Inside, 0x18b06/0x18b0c route 0fd9:005e straight to 0x18bf5, skipping all CPLD work; 0x18c02 'or al,0x2' then 0x18c04 'test dil,0x2' / 0x18c0a 'and al,0xfd' clears it again; write 0x18c25.

### 51. cpld_read+cpld_write  *(CONFIRMED)*

v = cpld_read(0x20) ; cpld_write(0x20, v | 0x10). IMPORTANT: pass NULL for the reader's third argument — it is an optional BOOLEAN* status out-parameter, not a value.

> 0x4329a call 0x65b10 with 'xor r8d,r8d' at 0x43292, then 0x432a4 'or al,0x10' / 0x432aa call 0x65a50. The r8=BOOLEAN* signature is from 0x65b2d 'mov rbx,r8' / 0x65b98 'test rbx,rbx' / 0x65b9d 'mov BYTE PTR [rbx],cl'.

### 52. i2c_write  *(CONFIRMED)*

mst_write(0, 0xB7, 0x00). Writes the WHOLE register to zero; HPD bit stays low.

> 0x432af 'jmp 0x4372a'; at 0x4372a 'xor r9d,r9d' / 0x4372d 'mov r8b,0xb7' / 0x43730 'xor edx,edx' / 0x43735 call 0x65080. I disassembled both sites.

### 53. i2c_read+i2c_write  *(CONFIRMED)*

MST3367_HDMI_INIT, in exact order: (a) v=mst_read(2,0x01), mst_write(2,0x01,(v&0x0F)|0x60) ; (b) v=mst_read(2,0x04), mst_write(2,0x04,v|0x01) ; (c) mst_write(2,0x06,0x08)  [BARE WRITE, no preceding read] ; (d) v=mst_read(2,0x09), mst_write(2,0x09,v|0x20) ; (e) v=mst_read(0,0x54), mst_write(0,0x54,v&0xEF) ; (f) v=mst_read(0,0xAC), mst_write(0,0xAC,v|0x80) ; (g) v=mst_read(0,0x00), mst_write(0,0x00,v|0x80) ; (h) v=mst_read(0,0xCE), mst_write(0,0xCE,v|0x80) ; (i) v=mst_read(0,0xCF), mst_write(0,0xCF,(v&0xFA)|0x02) ; (j) v=mst_read(0,0x00), mst_write(0,0x00,v&0x7F) ; (k) d=mst_read(0x80,0xD0), c=mst_read(0x80,0xCF), mst_write(0x80,0xD0, d & 0xFC), mst_write(0x80,0xCF, c & 0x7F).

> 0x18c40-0x18ead. Steps a-j at 0x18c67..0x18d8e. Step k is the ARM AT 0x18ded, not the one at 0x18d9c — 0x18d93 'cmp DWORD PTR [rbp+0x8e98],0' / 'jbe 0x18ded' selects between two different D0/CF updates and the ==0 arm is ours ([dev+0x8e98] = CustomCompanyEndoCamProperty, default 0, not in the INF). I disassembled 0x18d93-0x18e86 and resolved its table: idx = [dev+0x78f4] % 6 via 'mov eax,0xaaaaaaab / mul edi / shr edx,2', table {06,00,04,03,07,01} written to [rsp+0x20..0x25] at 0x18e03-0x18e28, and [dev+0x78f4]=CustomAnalogVideoInputBandwidthProperty defaults to 1 ('cmovne ecx,esi', esi=1 at 0x24ca3) -> v = 0x00 -> D0 &= 0xFC and CF &= 0x7F. Steps (g) and (j) touch the page-select register itself and may be dead, since the accessor rewrites reg 0x00 before every transfer — harmless, keep them.

### 54. delay  *(CONFIRMED)*

delay 300 ms

> 0x43742 'mov eax,[rdi+0x7104]' / cmp 2 / cmp 3 / 0x43750 'jne 0x43c80'; at 0x43c80 [dev+0x8ea4] (= CustomCompanyCodyyProperty, NOT Elgato — name pairing at 0x26ee9 -> 0x299b50 by stack slot [rsp+0x3ec]) is 0 so no early return; 0x43ca5 'mov ecx,0x12c' / 0x43caa call 0x7ac60; returns 1 at 0x43cbd.

### 55. cpld_write  *(CONFIRMED)*

cpld_write(0x20, 0x05). This is the route setter's own write and it CLEARS the bit4 you set in step 51 — that is what Windows does.

> Back in 0x6f230 after MST3367_HwInitialize returns. I traced the fallthrough: 0x6f4c6 call -> 0x6f4cb PID chain -> 0x6f4fb 'cmp ecx,0x5e / je 0x6f524' -> 0x6f527 call 0x4d130 -> 0x6f52c call 0x6db40 -> 0x6f563 -> VID/PID 0x6f590 'je 0x6f622' -> 0x6f629 'je 0x6f73b' -> reg-0x20 write at 0x6f75c-0x6f811 (i2c payload [0x20][0x05] staged at 0x6f7c7/0x6f7cf, sent 0x6f7dc).

### 56. cpld_read+cpld_write  *(CONFIRMED)*

v = cpld_read(0x20) ; cpld_write(0x20, v & 0xEF). Net result: CPLD 0x20 == 0x05.

> 0x6f85d read, 0x6f862 'test edi,edi' with edi = arg4 = [dev+0x7118] = 0 -> 'and al,0xef'; 0x6f875 write. (edi==1 would set bit4 instead.)

### 57. note  *(SPECULATIVE)*

OPTIONAL / NOT DECODED, and I recommend SKIPPING both on a first attempt: (a) the IT6612 HDMI-transmitter init at 0x4d130 (I2C slave 0x9A) — this is the pass-through OUTPUT port, not the capture path; (b) MST3367_ADJUST_CSC_TABLE reached via 0x6db40 with edx=1, which loads colour-space coefficients from [dev+0x6d94/0x6e14/0x6e94/0x6ed4/0x6f14] procamp values. Neither is required for the receiver to lock; (b) at worst affects colour.

> 0x6f527 call 0x4d130; 0x6f534 call 0x6db40 with edx=1, which at 0x6db87 'test edx,edx / jne 0x6db90' skips 0x69fc0, then [cfg+0x17fc]==0 -> 0x6dbff -> 0x6dc32 'call 0x16eb0 <MST3367_ADJUST_CSC_TABLE>' and 0x6dc37 'test edi,edi / jne 0x6e181' skips 0x69f00. I disassembled this but did not decode 0x16eb0's register writes.

### 58. i2c_read  *(CONFIRMED)*

STREAM-START BLOCK BEGINS. AB_old = mst_read(0, 0xAB) ; P2_27_old = mst_read(2, 0x27). Keep both.

> CY3014_HwAnalogComponentsInitialize, entry 0x6e1b0 (called 8x from CStreamPin::CreateStream at 0x15a0c..0x15a94). Reads at 0x6e370 and 0x6e37d.

### 59. i2c_write  *(CONFIRMED)*

mst_write(0,0xAB,(AB_old & 0x7F) | 0x80) ; mst_write(2,0x27,0xFF) ; mst_write(0,0xB3,0xFF) ; mst_write(1,0x24,0x41) ; mst_write(1,0x25,0x00) ; mst_write(1,0x26,0x00) ; mst_write(1,0x27,0x00) FIVE TIMES

> 0x6e39d, 0x6e3aa, 0x6e3ba, 0x6e3ca, 0x6e3da, 0x6e3ed, 0x6e400 (loop, 'dec rbx' at 0x6e410). Note 0x24 gets 0x41 here, unlike the 0x40 in step 27.

### 60. i2c_write  *(CONFIRMED)*

mst_write(0,0xAB, AB_old & 0x7F) ; mst_write(2,0x27, P2_27_old) ; mst_write(0,0xB3,0x00)  — restore the three saved/reset registers.

> 0x6e415, 0x6e426, 0x6e437.

### 61. poll  *(CONFIRMED)*

Up to 10 iterations: v = mst_read(2, 0x0E) ; if (v & 0x07) != 0 stop; else delay 5 ms. Timeout here is NON-FATAL — the driver only logs and continues.

> 0x6e452-0x6e472, verified by my own disassembly: 'test al,0x7 / jne 0x6e476', 'mov ecx,0x5 / call 0x7ac60', 'cmp ebx,0xa / jl 0x6e452'.

### 62. poll  *(CONFIRMED)*

Up to 500 iterations: v = mst_read(2,0x0E) ; mst_write(2,0x0E, v)  [write-1-to-clear] ; if (v & 0x07) == 0 stop; else delay 5 ms. Timeout also NON-FATAL.

> 0x6e490-0x6e4c8: read 0x6e498, write-back same value 0x6e4ac, 'test bl,0x7 / je 0x6e4cc', 'mov ecx,0x5', 'cmp ebp,0x1f4 / jl 0x6e490'.

### 63. usb_setup  *(SPECULATIVE)*

SET_INTERFACE interface 0, alternate setting 2. Rationale: tier = 1 if scaled_rate <= T, else 2, else 3 when SuperSpeed and scaled_rate > 2T; T = 0x3E80000 (SuperSpeed) or 0x3E8000; scaled_rate = rate + ((rate * 0xB926FAB) >> 32) = rate * 1.045190; rate = width*height*fps, defaulting to 0x76A7000 (= 1920*1080*60 = 124,416,000) when the detected mode is still zero. 124,416,000 * 1.04519 = 130,038,318 which is <= 2*0x3E80000 = 131,072,000, so the tier is 2. If alt 2 yields nothing, walk 3 then 1 then 4.

> Arithmetic at 0x6e737-0x6e7a7 is CONFIRMED (0x6e756/0x6e75b/0x6e760 thresholds, 0x6e77f imul, 0x6e78d/0x6e795/0x6e799-0x6e7a7 tiering). What is SPECULATIVE is that this maps onto real alt settings on this board, because nobody read the descriptors. Alt 4 = BULK and alt 1/2/3 = isochronous is proven by the debug strings at 0x29d280 / 0x29d180 ('Run Bulk mode') versus 0x29d2c0 / 0x29d220 / 0x29d1c0 ('Run Isochronous mode, alternate setting = %d'), each printed right after the matching AllocateBusBandwidth call. Bulk (alt 4) is chosen only when [dev+0x7488]=CustomMunaulUsbPipeSelectProperty >= 2 (absent from the INF, default 0) or the host controller is ASMedia 1B21:1042/1142/1242, Etron 1B6F:7023 or VIA 1106:3432. Note Windows also puts the interface into alt 4 at pipe-creation time first (0x1437b 'mov bl,0x4', walking down on failure) before selecting the streaming alt.

### 64. read  *(CONFIRMED)*

Submit bulk/iso IN transfers on endpoint 0x83 and see whether bytes arrive. There is no further vendor command to issue — SET_INTERFACE is the last device-facing operation in the entire Windows start path.

> I re-checked every bRequest byte fed to SendVendorCommandRequest (0x13fd0) across the image: only 0xA0, 0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xEC exist, and none is issued from StartStream (0x164d0) or RunStream (0x16c70). CStreamPin::OnPinSetDeviceState (0x16d20) reaches hardware only through CreateStream -> 0x6e1b0, which is steps 58-63.

### 65. poll  *(CONFIRMED)*

HEALTH CHECK, run in a loop. Read the receiver's lock/timing state: v55 = mst_read(0,0x55) — the signal is valid only if (v55 & 0x3C) == 0x3C. If valid, Vtotal = ((mst_read(0,0x5B) & 0x07) << 8) | mst_read(0,0x5C) ; active width = (((mst_read(2,0x29) & 0x1F) << 8) | mst_read(2,0x28)) rounded up to even ; Hfreq = 1600000 / (((mst_read(0,0x57) & 0x3F) << 8) | mst_read(0,0x58)) ; Vfreq(0.1 Hz) = 1250000 / (((mst_read(0,0x59) & 0x3F) << 8) | mst_read(0,0x5A)) ; interlaced = bit3 of mst_read(0,0x5F). Stability gates the driver itself applies: Vtotal >= 0x96, Vfreq in [200,1500], and |Hfreq*1000/Vtotal - Vfreq| <= 5. WATCH THE PAGES: registers 0x55, 0x57-0x5C, 0x5F, 0x6A, 0x6B are page 0, but registers 0x28/0x29/0x47/0x4C are page 2 and register 0x01 is page 1.

> MST3367_HDMI_MODE_DETECT 0x18ec0. Gate 0x18f31 'and bl,0x3c / cmp bl,0x3c'. Vtotal 0x1910c-0x19121; page2 0x29/0x28 with even-rounding 0x19086/0x190f3/0x1912e; Hfreq 'mov eax,0x186a00' idiv at 0x19161; Vfreq 'mov eax,0x1312d0' idiv at 0x19189; interlace 0x19196. Stability 0x192e0/0x192ec/0x192fb. The page-1 fact for reg 0x01 is at 0x191b7 'mov r8b,0x1' / 0x191bd 'movzx edx,r8b' — dl (the page arg) is loaded FROM r8b, so it is page 1, reg 1. Value ordering: the caller extracts (packed>>32)&0xFFF at 0x470f8/0x47107 and compares it to 0x2d0 (720), 0x5a0 (1440), 0x77b..0x785 (1915-1925 -> 1920x1080) and 0x551 (1361), which proves that field is ACTIVE WIDTH, not Htotal.

### 66. note  *(CONFIRMED)*

THE GAP. If step 65 reports a locked, plausible mode but endpoint 0x83 stays silent, you have hit the undecoded part. MST3367_VideoFormatDetection at 0x43d20-0x49119 is the routine that programs the receiver for the detected timing; it contains 109 calls to the paged writer 0x65080 and 73 to the paged reader 0x65230, all mode-dependent, and it is called from exactly one place — 0x710e2, inside the control-panel analysis thread created at 0x6ffb1 and re-created at 0x6e516. Nothing in steps 1-64 replaces it. Decoding the (width, Vtotal) decision tree at 0x4711b-0x47244 and the configure block it dispatches into is the next required piece of work.

> I counted the calls myself over 0x43d20-0x49119 and confirmed the single call site by scanning the image for 'call 0x43d20'. The thread's dispatch guard is 0x703df 'cmp eax,0x4 / jne 0x710af' plus 0x710af 'cmp eax,0x5' and 0x710b8 'cmp eax,0x6' — so with [dev+0x7104]=0 it does run on Windows, and 0x710c1 'cmp DWORD PTR [r12+0x17fc],0 / je 0x710df' routes to 0x43d20 rather than the analog 0x66c30.

### 67. note  *(SPECULATIVE)*

CHEAPEST EXPERIMENT IF NOTHING STREAMS, in priority order: (1) mst_write(0,0xB7,0x02) after step 63 — Windows leaves this at 0x00 but if bit1 is this board's HPD line the source will never send TMDS; (2) cpld_read(0x20) and check bit 0 while idle versus while Windows is streaming — the mode-configure routine clears bit0 before reprogramming the receiver (0x46230 'and r8b,0xfe') and restores it after (0x4703d), which is suggestive of a data-path gate, though the init value 0x05 already sets it; (3) try alt 3, then alt 1, then alt 4 (bulk).

> (1) is my own finding, see assessment. (2) the bracket is real but note it is skipped wholesale by 0x46210 'je 0x47062', and the claim that nothing else touches bit0 is false — cpld_write(0x20,0x05) sets it. So bit0 as 'the' gate is an inference, not a proven fact.

## Still unknown

- USB DESCRIPTORS. Nobody ever read them. How many alternate settings interface 0 has, which are isochronous and which is bulk, and endpoint 0x83's transfer type and wMaxPacketSize per alt are all unknown. The driver's own tier arithmetic is fully decoded, but the mapping onto real alts is assumed. Run `lsusb -v -d 0fd9:005e` before anything else.
- THE PAYLOAD FORMAT ON ENDPOINT 0x83. Completely unanalysed. It is consumed by CY3014.X64.AX (the KS/DirectShow filter), which no analyst opened. It may be raw frames, a framed/packetised container, or something needing a header parse. Getting bytes out is not the same as getting frames.
- THE PER-MODE MST3367 CONFIGURE (0x43d20-0x49119, ~21 KB, 109 paged writes + 73 paged reads). This is the single largest missing piece and the most likely reason a device that completes every step above still emits nothing.
- WHETHER PAGE0 REGISTER 0xB7 BIT 1 IS ACTUALLY THE HPD LINE ON THIS BOARD. The driver ends with 0xB7 = 0x00 on the HDMI route, and on 0fd9:005e it explicitly skips the CPLD GPIO port-1 writes that drive HPD on sibling boards. So HPD is probably held elsewhere (CPLD or MCU), but I cannot prove it from this binary.
- SEMANTICS OF bRequest 0xC2 (zero-length OUT, issued unconditionally at 0x6fd47, result discarded). It is the most suspicious 'do something' command in the init path. The FX3 firmware is not in this package (no .hex for 005e), so it cannot be resolved from these files.
- THE FIVE DON'T-CARE BYTES OF THE 0xEC PAYLOAD (offsets 2,3,4,6,8). Windows sends uninitialised stack there, which strongly implies the firmware ignores them, but the meaning of the whole 10-byte structure B8 22 ?? ?? ?? C0 ?? 00 ?? CA is unknown.
- THE HD60 S's ACTUAL iManufacturer / iProduct DESCRIPTOR BYTES ([dev+0x120]+0xe and +0xf). These select between two different tails: if they are (0x5f, 0x05) then step 33 must write 0xB3 = 0x14 instead of 0x00, and step 49/50 must follow 0x4358a (hot-plug 0x30 and CPLD 0x20 bit4 CLEARED) instead of 0x43275 (hot-plug 0x03 and bit4 SET). The plan assumes they do not match, by elimination — 0fd9:005e is explicitly listed in every VID/PID fallback chain, and those chains are only reachable when the byte-pair lists miss. Check with lsusb -v; this is a one-line confirmation that de-risks three steps.
- WHETHER THE INTERRUPT-IN REGISTER SCANNER MUST RUN. The 0xC6 scanner uploads a 14-entry descriptor table and returns polled register values on the interrupt IN pipe. I include only the 0xC6 capability probe, not the scanner. It looks like telemetry rather than a gate, but nobody located the code that parses the interrupt-IN payload, so its wire format and necessity are unverified.
- WHETHER STREAMING NEEDS ANY EQUIVALENT OF THE FX3 RESET THAT PID 0x005E SKIPS. CY3014_HwReset jumps over its whole bRequest-0xC1 register-0x2D block for this PID, and HwInitialize skips the 0xC5 probe. Nobody established whether an equivalent reset happens elsewhere for this PID or whether it genuinely is not needed.
- THE IT6612 TRANSMITTER INIT (0x4d130) AND THE CSC TABLE LOAD (0x16eb0). Both are on the Windows path and both write registers; neither was decoded. Assumed irrelevant to capture (the IT6612 is the pass-through output; the CSC affects colour), but that is an assumption.

## Risky steps

- STEP 6 (fpga_write 0x34 = 0x41) starts/releases the MCU. If the device is not freshly plugged — e.g. another driver already brought it up, or a previous run of your own tool left it mid-init — re-driving this can wedge the MCU and the poll in step 7 will time out. Recovery is a physical re-plug (or a USB port power cycle). Always start from a freshly enumerated device and treat a step-7 timeout as 'unplug and retry', not as 'push on regardless'.
- STEP 16 (cpld_write 0x10 = 0xFE) programs CPLD GPIO DIRECTIONS, where 0 means OUTPUT. This drives pin 0 of port 0 as an output. The value comes straight out of the driver for this exact PID so it is safe as written, but do NOT experiment with other values here: turning an input into an output on a pin that something else is driving means two drivers fighting on a net, which is the one class of mistake in this plan that can do physical damage. Same caution for any write to CPLD 0x11.
- STEPS 50-52 leave HPD (page0 0xB7) at 0x00, matching Windows. If bit 1 really is the HPD line and something outside the driver normally asserts it, the source will see no sink and you will get a locked-looking receiver with no TMDS. Conversely, forcing 0xB7 |= 0x02 when the board expects HPD held by the CPLD could make the source and the board disagree about hot-plug state and require a cable re-plug to recover. Try it, but treat cable re-plug as part of the retry loop.
- STEP 26 must run AT MOST ONCE per process. Windows gates it on a driver-global one-shot byte in .data (0x3ada91), not on anything per-device. Re-running the 0x25/0x26 zeroing plus the five page-1 0x27 reads on every open is a behavioural divergence from Windows whose effect is unknown.
- STEP 63 (SET_INTERFACE) is the point at which the device starts consuming isochronous bus bandwidth. Getting the alt wrong will usually just fail the SET_INTERFACE or produce zero-length packets rather than break anything, but on a shared bus an over-large iso allocation can starve other devices on the same root hub. Prefer a dedicated port while bringing this up.
- GENERAL: every step here is a control transfer that the MCU proxies onto an I2C bus. Any step that reads a value, modifies it, and writes it back (steps 19, 24, 31, 34, 36, 39-41, 43, 45-47, 50, 51, 53, 56) will corrupt the register if the read fails, because the failure return of the paged reader is 0xFF (0x652fc / 0x65ba4) and it is indistinguishable from a genuine 0xFF. Check the transfer status on every read and abort rather than writing back a masked 0xFF.

## Assessment

```
ONE ORDERED BRING-UP PLAN FOR ELGATO HD60 S (0fd9:005e), LINUX USERSPACE.

=== WIRE PRIMITIVES (all CONFIRMED by my own read of 0x19ca0, 0x65080, 0x65230, 0x65a50, 0x65b10, 0x62c00, 0x62c40, 0x627f0, 0x62880) ===
fpga_write(reg,val) : bmRequestType 0x40, bRequest 0xC1, wValue=(val<<8)|reg, wIndex=0, no data stage.  [0x62c00]
fpga_read(reg)->u8  : bmRequestType 0xC0, bRequest 0xC1, wValue=reg, wIndex=0, 1 byte IN. 0xFF on failure. [0x62c40]
mcu_write(cmd,data) : bmRequestType 0x40, bRequest 0xC0, wValue=0x5000|cmd, wIndex=0, data OUT.  [0x627f0]
mcu_read(cmd,n)     : bmRequestType 0xC0, bRequest 0xC0, wValue=0x5000|cmd, wIndex=0, n bytes IN. [0x62880]
i2c_write(slave,b[]): if slave in {0x88,0x98,0x9c} -> mcu_write(slave, b[]); else -> mcu_write(0x66, [slave]+b[]).  [0x19d5e-0x19da4]
i2c_read(slave,w[],n): mcu_write(0x66, [slave|1, n] + w[]) ; then mcu_read(0x66, n).  [0x19d32-0x19d57, 0x19dc6-0x19dec]
mst_write(page,reg,v): i2c_write(0x9c,[0x00,page]) then i2c_write(0x9c,[reg,v])  -> two mcu_writes on wValue 0x509C. [0x6515a, 0x65139]
mst_read(page,reg)  : i2c_write(0x9c,[0x00,page]) then mcu_write(0x66,[0x9D,0x01,reg]) ; mcu_read(0x66,1). [0x6533b, 0x652cd]
cpld_write(reg,v)   : mcu_write(0x98,[reg,v])   -> wValue 0x5098. [0x65a8b/0x65aa7]
cpld_read(reg)      : mcu_write(0x66,[0x99,0x01,reg]) ; mcu_read(0x66,1). [0x65b58]
delay(ms)           : 0x7ac60, imul ecx,ecx,0xffffd8f0 (= ecx * -10000 x 100ns) = milliseconds.
NOTE: "page 0x80" on the MST3367 is not a 5th page — it is page-select register 0x00 written with bit7 set, i.e. a shadow bank.

=== TWO FINDINGS OF MY OWN THAT CHANGE THE PLAN ===
(1) HPD ENDS LOW, NOT HIGH. The edid analyst's headline recommendation ("a Linux driver MUST assert HPD, page0 0xB7 |= 0x02, as MST3367_HwInitialize does at 0x4276e") is only half the function. On the 0fd9:005e path the last three 0xB7 operations are: assert at 0x42fbd (mid-function), then MST3367_TMDS_HOT_PLUG(3) at 0x4328d which CLEARS bit1 (flags bit1 set -> 'and al,0xfd' at 0x18c0a), then an unconditional page0 0xB7 = 0x00 at 0x43735 immediately before MST3367_HDMI_INIT. I enumerated every 'mov r8b,0xb7' in the whole .text: 18bf5, 18c1d, 4235b, 42368, 4256a, 42577, 4276e, 4277b, 42fab, 42fb8, 436ed, 436fa, 4372d, 439a1, 439ae. None of them is inside MST3367_VideoFormatDetection (0x43d20-0x49119) and none is inside CY3014_HwAnalogComponentsInitialize. The TMDS_HOT_PLUG(0x11)/(0x00) assert calls at 0x43885/0x43894 sit behind 'cmp [rdi+0x7104],2 / cmp 3 / jne 0x43c80' at 0x43748-0x43750, i.e. the ANALOG route only; with the stock [0x7104]=0 they never run. Conclusion: Windows leaves MST3367 page0 0xB7 = 0x00 on this device, so either bit1 is not the board's HPD line (HPD is likely held by the CPLD/MCU in hardware, consistent with the CPLD reg 0x01/0x11 GPIO block being explicitly SKIPPED for 0x5e at 0x426a8 and 0x18b0c) or HPD is asserted outside the driver. The plan matches Windows and ends with 0xB7=0x00; trying 0xB7 |= 0x02 afterwards is listed as the single cheapest experiment.

(2) THE BRIEFING'S TWO DEVICE-FIELD LABELS ARE BOTH OFF BY ONE SLOT, and I proved it by stack-slot dataflow rather than by proximity. At 0x26b15 'lea r8,[rsp+0x3a4]' + 0x26b1d 'lea rdx,#0x299fb0' feed the call at 0x26b39 whose result [rsp+0x3a4] is stored to [rbx+0x8e64] at 0x26b5e; 0x299fb0 decodes as L"CustomCompanyS3Property". So [dev+0x8e64] = S3, and [dev+0x8e60] = 0x299fe0 = L"CustomCompanyAlconProperty", default 0 (cmovne ecx,r15d, r15d=0 from 0x2482b). Likewise [dev+0x78f0] = 0x296f60 = L"CustomAnalogVideoInputEqProperty" (default 1, 'cmovne ecx,esi' at 0x25c83 with esi=1) and [dev+0x78f4] = 0x296f10 = L"CustomAnalogVideoInputBandwidthProperty" (default 1). GameCaptureHD60S.inf sets none of them (I grepped). Consequence: the guard at 0x427ce 'cmp [rdi+0x8e60],1' is NOT taken (field is 0), so control goes to the 0x4280c [dev+0x78f0] cascade instead — where 0x78f0==1 hits the second arm at 0x42845 and writes page1 0x17/0x18/0x19 = 0x00 anyway. Same bytes on the wire, different route. And the HDMI_INIT tail at 0x18ded (the arm the analysts nearly dropped) resolves concretely: idx = [0x78f4] % 6 = 1, table {06,00,04,03,07,01} at [rsp+0x20..0x25] -> v = 0x00 -> page0x80 0xD0 &= 0xFC and page0x80 0xCF &= 0x7F.

=== PROBABILITY ===
Bytes appearing on endpoint 0x83 after this plan: ~50%.
Bytes that decode into recognisable video frames: ~15%.
MOST LIKELY BLOCKER, in order: (a) MST3367_VideoFormatDetection at 0x43d20-0x49119 is undecoded and is where the receiver actually gets programmed for the incoming timing — I counted 109 calls to the paged writer 0x65080 and 73 to the paged reader 0x65230 inside it, all mode-dependent, and it has exactly one caller (0x710e2, inside the control-panel analysis thread). Nothing in the init path I traced ever calls it. So a device that completes every step below can still emit nothing because the HDMI receiver was never told what mode to lock to. (b) The payload container on EP 0x83 is unknown — it is parsed by CY3014.X64.AX (the DirectShow/KS filter), which nobody analysed; it is not necessarily raw frames. (c) The alternate-setting/endpoint layout is a guess because nobody read the HD60 S descriptors.
FAVOURABLE: there is genuinely no "start GPIF/DMA" vendor command. I confirmed only bRequest 0xA0, 0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xEC exist across all SendVendorCommandRequest sites, and none is issued at stream start. SET_INTERFACE is very likely the whole trigger, which is the best news in this analysis.
```