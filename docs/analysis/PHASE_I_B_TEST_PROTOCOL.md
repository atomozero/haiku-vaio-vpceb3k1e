# Phase I.B hello-world test protocol

| | |
|---|---|
| **Status** | 📄 Reference |
| **Category** | Test protocol |
| **Target** | Sony Vaio VPCEB3K1E — Intel Ironlake (Gen5), GPU `0x0046` |
| **Updated** | 2026-07-19 |

[← Documentation index](../INDEX.md)

---


Procedure for running the Gen5 media-pipeline "lights on" probe on the VAIO VPCEB3K1E and interpreting the results. This is the document to follow when you're about to reboot the machine with the test-enabled accelerant.

---

## 0. Pre-flight: what this test does

The test-enabled accelerant, on the very first call to `intel_init_accelerant()` after the library is loaded, runs `media_pipeline_run_hello_test()`. The test:

1. Allocates 7 GTT buffer objects via `gpu_bo_alloc()` (batch, kernel, VFE state, IDRT, CURBE, output, marker).
2. Uploads the embedded one-instruction EU kernel (`kernels/hello_world.g4a` → `send ... thread_spawner EOT`).
3. Writes the `i965_vfe_state` blob, the interface descriptor, and sentinel values into the marker/output BOs.
4. Builds an 11-marker, 10-command batch into the batch BO per `MEDIA_PIPELINE_BRINGUP.md` §1.
5. Submits via the primary render ring using `MI_BATCH_BUFFER_START`.
6. Polls the last marker slot for up to 500 ms.
7. Dumps all GPU debug registers, marker slots, and key BO contents to the syslog.
8. Tears down.

A successful run proves **every link in the chain from CPU-visible ioctl through batch submission through EU thread execution to URB deallocation works on Gen5**.

---

## 1. Safety: what can go wrong and how to recover

### 1.1 Worst realistic outcome
The probe hangs the GPU. The screen freezes at the point of `intel_init_accelerant()`. On the VAIO this is typically during early boot, before app_server draws the desktop — you get a black or frozen splash screen. A subsequent boot will re-load the same accelerant and hit the same hang, producing a **soft boot loop** from the user's perspective.

The GPU hang does not damage the hardware and does not affect anything persistent on disk. Recovery is a file-system operation: remove or restore the accelerant override file, and the next boot loads the stock (packaged) accelerant again.

### 1.2 Recovery paths, from least to most invasive

**Path A — Haiku safe mode (try this first).**
At the boot menu hold SPACE, choose "Safe mode: Disable user add-ons". This skips loading anything under `/boot/system/non-packaged/add-ons/`, so the test accelerant is not loaded and the stock accelerant takes over. Once Haiku is up:
```sh
cd "/boot/home/Desktop/Sony Vaio VPCEB3K1E/intel_extreme/accelerant"
make revert-test
```
Reboot normally.

**Path B — Boot from a Haiku USB.**
If safe mode also hangs for any reason, boot from a Haiku install USB, mount the Vaio's disk, delete:
```
/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant
```
(or restore `intel_extreme.accelerant.revert-backup` to that name). Reboot.

**Path C — Remote SSH during early boot.**
Haiku's SSH server starts before app_server. Even if the display freezes, you may still be able to SSH in over the LAN and run `make revert-test`. Verify by noting the IP during a normal boot beforehand.

### 1.3 Safeguards already in the code
- The probe is **opt-in at build time**, not runtime. Removing the `-DMEDIA_PIPELINE_HELLO_TEST` flag and rebuilding disables it fully.
- `media_pipeline_run_hello_test()` has a `static bool sAlreadyRun` that prevents re-entry within the same library load. App_server restarts within a single session won't re-trigger the probe.
- `media_pipeline_submit_hello()` installs its batch in a dedicated 4 KB BO (`batch_bo`), not in the shared `sBatchBase` used by the render path. A hang inside the media batch does not corrupt render/BLT state.
- `media_pipeline_wait_output()` has a hard 500 ms timeout. A stall is observed, not blocked indefinitely.
- `make test-install` saves `intel_extreme.accelerant.revert-backup` before overwriting. `make revert-test` restores it.

### 1.4 Before you run: check the fallback
Do this **before** running `make test-install`:

1. Confirm the current working accelerant is at `/boot/system/non-packaged/add-ons/accelerants/intel_extreme.accelerant`. If not present, then the system is currently using the packaged version and a rollback is "remove the override".
2. Note the machine's IP address (from `ifconfig` or a router GUI) in case SSH recovery is needed.
3. Verify you can reach Haiku's boot menu (hold SPACE on power-on, confirm the menu appears).
4. Know the physical media from which you could boot Haiku if Path A fails (a USB with Haiku install).

---

## 2. Build procedure

Two commands.

```sh
cd "/boot/home/Desktop/Sony Vaio VPCEB3K1E/intel_extreme/accelerant"

# 1. Build the gen4asm tool (one-time, if not already done).
make -C ../tools/gen4asm

# 2. Build the test-enabled accelerant.
make test
```

`make test` does:
- `make clean` (wipes objects, forces a fresh rebuild)
- `make all CFLAGS_COMMON="... -DMEDIA_PIPELINE_HELLO_TEST"`
- Assembles `kernels/hello_world.g4a` → `kernels/hello_world.g4b.gen5` via our ported `gen4asm`
- Compiles everything including `media_pipeline.cpp` with the test hook enabled
- Links `../intel_extreme.accelerant`
- Prints a summary banner with next-step commands

Expected result: no compile errors, no link errors, a binary of ~223 KB at `../intel_extreme.accelerant`.

### 2.1 Verify the right binary was produced
```sh
nm accelerant.o | grep hello_test
```
Expected output:
```
                 U _Z29media_pipeline_run_hello_testv
```
The leading `U` means "undefined reference" — i.e. the call site in `intel_init_accelerant()` is linking against the media-pipeline function, which is exactly what we want. If there is no match at all, the flag didn't reach the compile unit and the probe is disabled.

---

## 3. Install procedure

```sh
make test-install
```

This:
1. Ensures `/boot/system/non-packaged/add-ons/accelerants/` exists.
2. Saves any existing `intel_extreme.accelerant` there as `intel_extreme.accelerant.revert-backup`.
3. Copies the freshly built test accelerant over.
4. Prints recovery instructions.

The override at `/boot/system/non-packaged/add-ons/accelerants/` takes precedence over the packaged (system-wide) accelerant, so after reboot the system loads our test binary.

---

## 4. Run the test

```sh
shutdown -r -q
```

During boot, the moment `intel_init_accelerant()` runs, the probe fires. It should complete within ~1 second (500 ms timeout + a few hundred ms of setup/logging).

**If the machine completes boot to the desktop**: the probe did not hang. Proceed to §5.

**If the machine appears to freeze**: see §1.2 recovery.

---

## 5. Capture and read the log

After a successful boot:
```sh
grep -E 'intel_extreme (media|gpu_bo|gpu_debug)|HELLO-WORLD' \
    /boot/system/var/log/syslog \
    | tail -200 > /tmp/hello_test_log.txt

cat /tmp/hello_test_log.txt
```

The banners `HELLO-WORLD MEDIA PIPELINE TEST — start` and either `HELLO-WORLD TEST: PASSED` or `HELLO-WORLD TEST: TIMEOUT` bracket the relevant section. Copy the whole block for analysis.

---

## 6. Interpreting the outcome

### 6.1 SUCCESS: all markers fire
```
[10] after MI_FLUSH #2                0xbeef000a  OK
==================================================
  HELLO-WORLD TEST: PASSED
  EU array executed a kernel authored by us.
==================================================
```
Plus `INSTDONE=0xffffffff` ("all done") in the `post-complete` register dump.

**Meaning**: the entire media pipeline works. The 10-command sequence is correct, the VFE state blob is valid, the interface descriptor format is correct, the EU fetched and executed our authored kernel, and the thread reached EOT so VFE could reclaim its URB entry. Phase I.B "lights on" milestone achieved.

**Next step**: disable the probe (`make clean && make && make install`), then move to Phase 1.2 (48-thread parallel dispatch) or Phase 2 (start porting MPEG-2 decoder from libva).

### 6.2 PARTIAL: N markers fire, then timeout
```
[ 4] after URB_FENCE                  0xbeef0004  OK
[ 5] after STATE_BASE_ADDRESS         0xdeadbeef  NOT REACHED
[ 6] after MEDIA_STATE_POINTERS       0xdeadbeef  NOT REACHED
...
  HELLO-WORLD TEST: TIMEOUT (500 ms)
```

**Meaning**: the commands up to slot N completed; the command between slot N and slot N+1 is where the pipeline stalled or failed. Cross-reference the marker slot name with `MEDIA_PIPELINE_BRINGUP.md §1`:

| Last OK marker | Command that failed |
|---|---|
| `START` | `MI_FLUSH` #1 — ring probably broken |
| `after MI_FLUSH #1` | `3DSTATE_DEPTH_BUFFER` — the NULL depth quirk |
| `after 3DSTATE_DEPTH_BUFFER` | `PIPELINE_SELECT` — media pipeline entry |
| `after PIPELINE_SELECT` | `URB_FENCE` — URB partitioning |
| `after URB_FENCE` | `STATE_BASE_ADDRESS` — Ironlake 8-DWORD variant |
| `after STATE_BASE_ADDRESS` | `MEDIA_STATE_POINTERS` — our VFE state BO layout |
| `after MEDIA_STATE_POINTERS` | `CS_URB_STATE` — CURBE entry allocation |
| `after CS_URB_STATE` | `CONSTANT_BUFFER` — CURBE load |
| `after CONSTANT_BUFFER` | `MEDIA_OBJECT` — **thread dispatch** (most interesting hang) |
| `after MEDIA_OBJECT` | `MI_FLUSH` #2 — EOT signaling / VFE drain |

Cross-reference with the `post-timeout` register dump:

- `INSTDONE` decoded stalled stages: which pipeline unit is waiting? If `URB` is in the list, URB partitioning is underflowing. If `IS` (VS unit on Gen5) is in the list and we stopped at `MEDIA_OBJECT`, the EU is waiting for a URB entry that VFE never handed out. If `(all done)` and we still timed out, the ring itself didn't advance (MMIO TAIL write didn't land, or the kernel driver's ring management is off).
- `IPEHR` decoded opcode: confirms which command the parser was processing. Should match the "what failed" column above.
- `ACTHD`: shows the exact DWORD offset in the batch that was being executed. Use `gpu_debug_hexdump_bo(&ctx.batch_bo, 0, 80)` (already in the post-mortem output) to read back what we wrote at that offset, compare against what `MEDIA_PIPELINE_BRINGUP.md` §3 says should be there.

### 6.3 Zero markers fire
```
[ 0] START                            0xdeadbeef  NOT REACHED
```

**Meaning**: the ring never executed our batch at all. Either the batch BO is at an invalid GTT offset, the `MI_BATCH_BUFFER_START` from the ring was not recognized, or the ring's TAIL write never reached the GPU. Check:
- `gpu_bo_dump_live()` output: are the GTT offsets reasonable (within the aperture)?
- `RING: HEAD=... TAIL=... CTL=...` in the register dump: did TAIL advance past the `MI_BATCH_BUFFER_START` DWORDs?
- If TAIL did NOT advance: problem is in `QueueCommands` or the ring itself, not in our media pipeline code.
- If TAIL did advance but HEAD is stuck: the ring saw the batch jump but the batch contents are unreachable (wrong GTT mapping).

### 6.4 SUCCESS banner but INSTDONE has stalled stages
Rare. Means the ring drained but some EU stage never reported done. Usually indicates a half-executed previous batch (e.g. from the 3D render path) that didn't fully drain before the media probe ran. Ignore on first run; if it repeats, add a `gpu_debug_dump_registers("very early")` at the top of `intel_init_accelerant` to see baseline state.

---

## 7. Rolling back

After a successful or failed test run, always revert before normal daily use:

```sh
cd "/boot/home/Desktop/Sony Vaio VPCEB3K1E/intel_extreme/accelerant"
make revert-test
shutdown -r -q
```

`make revert-test` restores `intel_extreme.accelerant.revert-backup` if it exists, or removes the override entirely if not (falling back to the packaged accelerant). The `.revert-backup` file persists across runs, so running `make test-install` again simply rewrites it to whatever the new previous-state was — always safe.

---

## 8. What to report back

If the test fails in an interesting way, the useful data to capture for the next debugging session is:

1. The full `HELLO-WORLD` block from the syslog (banner to banner).
2. The `gpu_bo_dump_live` output (tells us the GTT layout).
3. The pre-submit and post-timeout register dumps.
4. The marker dump table.
5. The `hexdump media:batch` output (shows what was actually written to the batch BO).
6. The `hexdump media:vfe_state` and `hexdump media:idrt` output (shows our CPU-written state blobs).

All of this is already emitted by `media_pipeline_run_hello_test()`; just grab the syslog block.

---

## 9. After a successful first run

Once the hello-world test passes, the immediate next steps are:

1. **Disable the probe**: `make clean && make && make install` (the default target, no flag) and reboot. Normal operation restored.
2. **Celebrate briefly**: this is the moment the project crossed from "modeset-only driver" to "GPU runs our code". Non-trivial.
3. **Write `PHASE_I_B_REPORT.md`** with the pasted syslog output for the record, just like `PHASE_I_A_REPORT.md`.
4. **Start Phase 1.2**: extend `media_pipeline` to dispatch 48 threads via `MEDIA_OBJECT_WALKER` and verify the thread payload differentiation. This reuses the same infrastructure we already have.
5. **Plan Phase 2**: begin reading `libva_intel/src/i965_media_mpeg2.c:867–932` in depth — that's the function that builds the MEDIA_OBJECT sequence for an actual MPEG-2 slice. The surface state and binding table setup we skipped for hello world becomes mandatory there.

The hello-world test is the smallest possible step that, if it works, validates the largest amount of infrastructure. Every subsequent Phase 1.x milestone is incremental over it.
