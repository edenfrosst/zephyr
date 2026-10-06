Title: RFC: Rework the CRC driver API and subsys/crc

<!--
Paste each "###" section into the matching field of the RFC form (the form adds the headings).
Permalinks pinned to upstream main 8f62a4ab82b5c2121c088842d3233f75b603ce35.

cc when posting:
  @thenguyenyf        CRC maintainer (subsys/crc and drivers/crc)
  @mathieuchopstm     #108761 discussion, STM32
  @fkokosinski        #117449 (normal-form polynomials to drivers)
  @RichardSWheatley   Ambiq driver; can answer the RESULT complement question
  @Holt-Sun           NXP CRC driver changes
  @carlescufi         Arch WG notes on #74977 / #87557 (crc_ctx overhead question)
  @pdgendt            #74977 / #87557 review (generic begin/update/finish, -ENOTSUP, HW/SW selection)
  @Sunil-Hegde        TI MSPM0 CRC driver (#118812, in review)

TODO before submitting: confirm the continuation problem on nucleo_f767zi (same programmable
CRC block as L4/U5). The SoC dtsi already chooses zephyr,crc but leaves &crc disabled, so add an
overlay with `&crc { status = "okay"; };` -> CRC_HW_HANDLER=y, CRC16_CCITT/CRC32_IEEE use hardware.
Software reference: the same build with CONFIG_CRC_HW_HANDLER=n.
  - crc16_ccitt(): byte-at-a-time chain (as modem_ppp.c does) vs one call over the whole buffer
  - crc32_ieee_update(crc32_ieee(a), b) vs crc32_ieee(a||b)
  - compare both against the software result (same build with CONFIG_CRC16_CCITT=n etc.)
Expected per RM0456 §24.3.3: the chained results differ. If they match, drop the
"wrong results" claim from the Problem Description and revisit the init-domain reasoning.

init-domain evidence, for replies if asked:
  STM32/SF32LB: reflection on input/output only; RESET copies INIT into DR (RM0456 §24.3.3,
    §24.4.3; RM0351 §15.3.3; UM5201 §12.2.7) -> INIT unreflected
  Silabs: register LSB-first, INIT copied as-is (EFR32xG24 RM §14.3.2, §14.5.5) -> reflected
  Renesas: seed written into CRCDOR, reflected in LSB-first mode (RA6M1 §36.2.4; Figure 36.2
    in §36.3.1 = CRC-16/KERMIT) -> reflected

Bugs found while surveying, to be fixed separately (moved out of Concerns; post as a
follow-up comment or separate issues):
- RA0E1 silently computes nothing for polynomials it lacks (RA0E1 UM §24.2.1), and computes reflected results when asked for non-reflected ones, because the FSP's checks are compiled out.
- The Renesas driver sends `CRC16_ITU_T` and `CRC16_ANSI` down its [32-bit path](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_renesas_ra.c#L148-L153).
- The Renesas driver complements a `CRC32_IEEE` result in `update`, then [reuses it as the next seed](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_renesas_ra.c#L174), so `CRC32_IEEE` over more than one `update` is wrong.

Implementation-time questions (moved out of Concerns; settle during implementation):
- Does Ambiq's `SECURITY->RESULT` read back complemented? The HAL doesn't say and the datasheet isn't public. @RichardSWheatley, can you confirm? Otherwise a run of `tests/drivers/crc` on `apollo510_evb` will show it.
- How do the NXP blocks load the seed when reflecting? Kinetis/MCX writes it to `CRC_DATA` with `CTRL[WAS]` set (KE1xF RM §31.3.1). The LPC engine applies "a selected bit order" (LPC55S6x UM §46.6.2). Every example in both manuals uses a symmetric seed. Proposal: decide from the asymmetric-seed test below.
-->

### Problem Description

The CRC driver API's [`struct crc_ctx`](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/include/zephyr/drivers/crc.h#L141-L154) does not cleanly describe a CRC operation, and drivers interpret it differently:

- **`type` duplicates width + polynomial:** Drivers use it to pick a width and/or whitelist polynomials, so hardware that accepts any polynomial (NXP Kinetis/MCX, Silabs 16-bit) can't run algorithms outside the `crc_type` enum.
- **The final XOR isn't part of the API:** Drivers special-case `CRC32_IEEE` in different places ([STM32](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_stm32.c#L224-L226), [Renesas](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_renesas_ra.c#L194-L196), [Silabs](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_silabs_gpcrc.c#L91), [SF32LB](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_sf32lb.c#L91), [NXP](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_nxp.c#L94), [NXP LPC](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_nxp_lpc.c#L89)), and no other algorithm can request one.
- **Continuing a CRC through the hardware gives wrong results:** `sys/crc.h` [supports using the seed](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/include/zephyr/sys/crc.h#L212-L213) to continue a CRC between calls (e.g. PPP's per-byte checksum in [`modem_ppp.c`](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/subsys/modem/modem_ppp.c#L36)). But the seed's bit order is undefined, so it's wrong on STM32 (#111132 reports PPP failing on STM32N6) and others (see `init` below).

In the user API (`sys/crc.h`, #108761):

- **Callers can't choose hardware or software:** A short packet header and a multi-KiB flash image may use the same algorithm. For short data, a software CRC is usually faster and avoids resource locks, but the hardware/software choice is made at build time.
- **Errors aren't propagated to the caller:** The `crc_hardware.c` functions [return 0 on failure](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/subsys/crc/crc_hardware.c#L69-L72), so unsupported operations (and busy hardware) aren't detectable.

### Proposed Change (Summary)

- Describe a CRC to drivers by its parameters: `{width, reflected, poly, init}`, with `poly` and `init` both in unreflected (MSB-first) form. Drop `type` and `state`. This also allows making the CRC description `const`.
- Do the per-result steps (final XOR, output reflection when it differs from input reflection, and the trailing bytes on peripherals that only take whole words) once in `subsys/crc`, to cover hardware differences.
- Make `crc_begin()` return `-ENOTSUP` for parameters the hardware can't handle, and take a timeout so a caller can get `-EBUSY` or `-EAGAIN` instead of blocking while the peripheral is in use.
- Let callers choose hardware or software at the call site, through either explicit `*_hw(dev, ...)` or explicit `*_sw(...)` functions in `sys/crc.h`. The existing functions keep their signatures and behaviour.

Ideally, I'd like this to make it in time for v4.6.

### Proposed Change (Detailed)

**Background:** this is the proposal [I said I'd write up in #108761](https://github.com/zephyrproject-rtos/zephyr/issues/108761#issuecomment-4422388820), when reviewers suggested a dedicated RFC. It also picks up points from earlier discussions: describing a CRC by its parameters rather than a per-algorithm enum (@mathieuchopstm, [#74977](https://github.com/zephyrproject-rtos/zephyr/pull/74977#discussion_r1697185160)), the `crc_ctx` overhead and multiple CRC engines (Architecture WG, [#87557, 2025-07-29](https://github.com/zephyrproject-rtos/zephyr/pull/87557#issuecomment-3133060643)), and choosing hardware or software per algorithm being "all or nothing" ([#87557](https://github.com/zephyrproject-rtos/zephyr/pull/87557#discussion_r2046235619)).

**Hardware survey:** What each peripheral can do according to its manual or HAL (Ambiq: according to the driver):

| Peripheral | Widths | Polynomial | Reflection | HW final XOR | Length unit |
|---|---|---|---|---|---|
| STM32 | 7, 8, 16, 32 | any (odd) | in/out independent | none | byte |
| SF32LB | 7, 8, 16, 32 | any | in/out independent | none | byte |
| Silabs GPCRC | 16, 32 | any 16-bit, 32-bit IEEE only | in/out independent | none | byte |
| NXP Kinetis/MCX | 16, 32 | any | in/out independent | all-ones only | byte |
| NXP LPC | 16, 32 | 0x1021, 0x8005, IEEE | in/out independent | all-ones only | byte |
| Renesas RA | 8, 16, 32 | 0x07, 0x8005, 0x1021, IEEE, CRC-32C | one setting for both | none | 4 bytes for 32-bit |
| Renesas RA0E1 | 16, 32 | 0x1021, IEEE | reflected only (in and out) | none | 4 bytes for 32-bit |
| Ambiq Apollo510 | 32 | IEEE only | reflected only (in and out) | unknown | 4 bytes |
| TI MSPM0 (#118812, in review) | 16, 32 | 0x1021, IEEE | one setting for both, driver switches it before reading the result | not used | byte |

**Driver API:**

```c
struct crc_cfg {
	uint8_t  width;     /* 7..32 */
	bool     reflected; /* reflect input and output */
	uint32_t poly;      /* unreflected, top bit is implicit */
	uint32_t init;      /* unreflected initial value */
};

__syscall int crc_begin(const struct device *dev, const struct crc_cfg *cfg, k_timeout_t timeout);
__syscall int crc_update(const struct device *dev, const void *buf, size_t len);
__syscall int crc_finish(const struct device *dev, uint32_t *raw);
```

- `crc_begin()` waits up to `timeout` for exclusive use of the device, returning `-EBUSY`/`-EAGAIN` as `k_sem_take()` does. The existing `sys/crc.h` functions pass `K_FOREVER`, which keeps today's behaviour for drivers that block. Ambiq and the TI MSPM0 driver in #118812 currently return `-EBUSY` instead of blocking, which the existing functions report as a CRC of 0. With `K_FOREVER` they wait instead.
- Unsupported parameters return `-ENOTSUP`. Currently, some drivers return `-EINVAL` for an unsupported polynomial (e.g. [NXP](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_nxp.c#L66-L68)).
- `crc_finish()` stores the register value in `*raw`, masked to `width` and before the final XOR.
- No `type`. Width is passed explicitly because a polynomial alone doesn't encode it (`0x07` could be 8-bit or 16-bit).
- No `state`. Every driver already holds a lock from `begin` to `finish`.

**`subsys/crc`:** Algorithms are described by the full [RevEng CRC parameter set](https://reveng.sourceforge.io/crc-catalogue/legend.htm) (`width`, `poly`, `init`, `refin`, `refout`, `xorout`):

```c
struct crc_algo {
	uint8_t  width;
	bool     refin;
	bool     refout;
	uint32_t poly;
	uint32_t init;
	uint32_t xorout;
};
```

Each algorithm is a preset macro that yields a pointer to a `const` compound literal, built on top of a member initializer, like the Bluetooth [`BT_LE_ADV_PARAM_INIT`](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/include/zephyr/bluetooth/bluetooth.h#L1134) presets:

```c
#define CRC_ALGO_CRC32_IEEE ((const struct crc_algo[]){ CRC_ALGO_INIT(32, 0x04C11DB7, ...) })
```

`enum crc_type`, `crc_by_type()` and the `*_INIT_VAL` macros stay as they are. `subsys/crc` maps a `crc_algo` onto `crc_cfg`, then:

- on peripherals with a 4-byte length unit, feeds the aligned part to the hardware and finishes the last 1–3 bytes in software from the raw result. This requires a small bitwise implementation that takes a `crc_algo`.
- bit-reverses the result when `refout != refin` (`RBIT` where available).
- applies `xorout` itself, so hardware complements such as NXP's `FXOR` are no longer used.

These steps run once per result, so doing them in software has a flat overhead, and drivers no longer implement them individually.

**`init`:** Different peripherals have different expectations for bit ordering, so each driver converts `init` to the form its hardware expects. Currently, the reflected `sys/crc.h` functions (e.g. `crc16_reflect(poly, seed)`) take the seed bit-reversed (in the same form as the result), so a previous result can be passed straight back in. `subsys/crc` reverses it before passing it to the driver.

- Currently, the bit ordering is undefined. STM32 and SF32LB load the seed as-given, but the polynomial is always unreflected. [Silabs](https://github.com/zephyrproject-rtos/zephyr/blob/8f62a4ab82b5c2121c088842d3233f75b603ce35/drivers/crc/crc_silabs_gpcrc.c#L121) bit-reverses *all* 16-bit seeds, regardless of reflection.

**`sys/crc.h`:** The existing functions are unchanged, and `CONFIG_CRC_HW_HANDLER` plus the per-algorithm `CONFIG_CRC*` options still decide at build time which of them use the hardware, bounded by `CRC_DRIVER_HAS_*`. `CRC_DRIVER_HAS_*` only controls which functions use the hardware, and doesn't limit what `crc_begin()` accepts. Call-site selection can come from either (or both) of two sets of explicit functions:

*1. Explicit hardware functions (`*_hw`)*, generic over `crc_algo`:

```c
int crc_compute_hw(const struct device *dev, const struct crc_algo *algo, const uint8_t *buf,
		   size_t len, uint32_t *out, k_timeout_t timeout);
int crc_compute_update_hw(const struct device *dev, const struct crc_algo *algo, uint32_t crc,
			  const uint8_t *buf, size_t len, uint32_t *out, k_timeout_t timeout);

/* e.g. */
ret = crc_compute_hw(dev, CRC_ALGO_CRC32_IEEE, buf, len, &crc, K_NO_WAIT);
```

- They take any `crc_algo`, so algorithms outside the Zephyr `crc_type` catalogue are reachable, and only two functions are added.
- `crc_compute_update_hw()` takes the previous result, and `subsys/crc` undoes the final XOR and output reflection before continuing. That gives one convention for every algorithm, where the existing functions differ (`crc32_ieee_update()` undoes the XOR itself, `crc16_ccitt()` takes the reflected register value).
- The caller passes the device, so there can be several instances and no dependency on `zephyr,crc`.
- They return an error code, so `-ENOTSUP` reaches the caller. They also take a timeout, so the caller can decide whether to wait for the peripheral or fall back to software.
- They are inline helpers over `crc_begin`/`crc_update`/`crc_finish`, not new syscalls.

*2. Explicit software functions (`*_sw`)*, for every algorithm:

```c
uint32_t crc32_ieee_sw(const uint8_t *buf, size_t len);
uint32_t crc32_ieee_update_sw(uint32_t crc, const uint8_t *buf, size_t len);
/* ... */
```

- The existing software implementations (the `__weak` functions in `subsys/crc/crc*_sw.c`) get public names with the existing signatures, so this only adds API.
- They can't fail, so no new error handling is needed.
- A generic `crc_compute_sw()` would use the bitwise implementation, which is slower than the table-driven per-algorithm functions unless it recognises the presets and dispatches to them.
- With `*_sw` alone, the hardware path stays behind the existing names, without a device argument or error codes.

The two aren't exclusive, but one may be enough. Either way, both implementations are callable in the same build (with `*_hw` alone, by leaving the existing functions on software). Tests can then run both on the same buffer and compare the results.

### Dependencies

This changes `drivers/crc/` (all in-tree drivers), `subsys/crc/`, `include/zephyr/drivers/crc.h` and `include/zephyr/sys/crc.h`, and updates `samples/drivers/crc` and `tests/drivers/crc` (the only in-tree users of the driver API outside `subsys/crc`). The driver API is experimental, so no deprecation period is needed. Users of `sys/crc.h` (modbus, DNS, FCB, …) are unaffected.

### Concerns and Unresolved Questions

- **`*_hw`, `*_sw`, or both?** Both give callers a per-call choice. `*_hw` adds two generic functions with error codes and a device argument. `*_sw` adds one function per algorithm, reusing the existing signatures and table-driven implementations (see Proposed Change).
- **Should the driver support `refin != refout`?** Proposal: no, `subsys/crc` fixes the output. If reviewers prefer, `crc_cfg` can carry both flags, and hardware that can't mix them returns `-ENOTSUP`.
- **Should a higher-priority caller be able to preempt a calculation in progress?** Some peripherals expose their internal state: STM32 can read the running CRC from `CRC_DR` and reload it through `CRC_INIT` (RM0456 §24.3.3), so a calculation could be saved and restored. Proposal: out of scope. The timeout on `crc_begin()` lets the caller choose to wait or use software.
- **Are three syscalls per computation too many under userspace?** Proposal: not for now. A one-shot syscall can be added later without changing anything above.

### Alternatives Considered

- **Per-algorithm hardware functions** (`crc32_ieee_hw()`, `crc16_ccitt_hw()`, …) mirroring the existing names. They read like the existing API, but add a function per algorithm and are thin wrappers over `crc_compute_hw()` with a preset. They could be added on top if people want them.
- **A length threshold in the existing functions:** Buffers shorter than a Kconfig-set length use software, longer ones use the hardware. No API change, but it only accounts for time, not contention for the peripheral. It's compatible with this proposal and could be added on top.
- **Runtime fallback in the existing functions:** (try the hardware, fall back on `-ENOTSUP`). Every unsupported call starts with a failing `begin`, which is a syscall under userspace. The build-time selection @thenguyenyf pointed to in #108761 avoids that.
- **Describe all capabilities in Kconfig:** (e.g. `HAS_CRC16_ANY`). Kconfig can't easily express "any 16-bit polynomial, but reflection only on both sides", or differences between parts that share a compatible, like RA0E1.
