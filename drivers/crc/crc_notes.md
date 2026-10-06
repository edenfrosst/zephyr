# CRC

crc hardware appears to come in two variants:

1. supporting generic polynomials (possibly with restrictions)
   - ex: stm32, sf32lb
2. supporting a selection of fixed polynomials
   - ex: nxp, renesas
3. a combination, supporting generic polynomials for certain sizes
   - silabs

## proposal (drivers)


### reorganize CRC Kconfigs

HAS_CRC16_ANY (etc.)

HAS_CRC16_CCITT (etc.)

ex. silabs would be HAS_CRC16_ANY and HAS_CRC32_IEEE


### change crc_ctx

- make crc_cfg (const struct) and crc_ctx(naming?) = {state, result}
  - do we need state?

- crc_cfg:
  - width (bits)
  - refin/refout
  - poly
  - seed
  - xor_out


## proposal (subsystem)

- keep existing API (back compat)
  - if a HW driver is available for an algorithm, use it
- add additional layer: crc{16,32,...}_sw functions always calculate in software

## revised proposal (claude)

### hardware capabilities (peripheral, not driver)

- stm32: width 7/8/16/32, any odd poly, refin/refout independent, no HW xorout (RM0456 §24.2, §24.3.3)
- sf32lb: width 7/8/16/32, any poly, any init, refin/refout independent (UM5201 §12.2.2)
  - no HW xorout: "must be handled by software" (§12.2.3)
  - byte granularity via CR_DATASIZE (§12.2.4)
  - driver doesn't expose CRC7
- silabs gpcrc: 16-bit any poly; 32-bit IEEE only; refin/refout independent (EFR32xG24 RM §14.1, §14.3.1)
  - native register is LSB-first -> explains the driver's inverted flags (§14.3.2: DATA reversed, DATAREV normal)
- nxp kinetis/s32: width 16/32, any poly (GPOLY); refin/refout independent (TOT/TOTR); HW xorout all-ones only (FXOR) (KE1xF RM §31.1, §31.2.3)
  - FXOR clear -> raw checksum readable (§31.3.4); intermediate value readable any time (§31.2.1)
- nxp lpc: fixed polys 0x1021, 0x8005, IEEE; refin/refout independent; HW xorout all-ones only (CMPL_SUM) (LPC55S6x UM §46.2, §46.6.1)
  - 8/16/32-bit writes -> byte granularity
  - also complements input data (CMPL_WR); not needed
- renesas ra: fixed polys 0x07, 0x8005, 0x1021, IEEE, 32C (RA6M1 §36.2.1)
  - one bit order setting (CRCCR0.LMS) for both directions
  - ra0e1: CRC-CCITT + CRC-32 only, LSB-first only (RA0E1 UM §24.1, §24.2.1)
    - unsupported GPS = "no calculation executed"; FSP check compiled out (BSP_CFG_PARAM_CHECKING_ENABLE 0) -> silent wrong result
    - Kconfig.renesas_ra advertises CRC8/16/32C for all RA parts -> driver needs per-part check (BSP_FEATURE_CRC_POLYNOMIAL_MASK, _HAS_CRCCR0_LMS)
    - crc node disabled on fpb_ra0e1 by default
  - 32-bit: data in 32-bit units only (RA6M1 §36.1, Table 36.1)
- ambiq: CRC32 IEEE only, refin = refout = true, length multiple of 4
  - unknown whether SECURITY->RESULT reads back complemented

### driver API

- crc_cfg (const):
  - width
  - reflected (refin == refout)
  - poly: normal form, implicit top bit
  - init
- drop `type`
  - drivers only use it for width, or to whitelist (width, poly)
  - width can't be inferred from a normal-form poly (0x07: 8- or 16-bit?)
- drop ctx `state`
  - device lock already serializes begin..finish
  - driver keeps progress internally; finish returns result
- finish returns raw register: masked to width, before xorout
- begin returns -ENOTSUP for unsupported cfg
- even-poly checks go away: a real poly in normal form is always odd
- compatibility: drivers/crc.h is @version 0.1.0 (experimental)
  - in-tree users: crc_hardware.c, samples/drivers/crc, tests/drivers/crc

### core (per-result work, out of drivers)

- lives in subsys/crc, alongside crc_hardware.c and the SW implementations
- xorout: one XOR, applied once in the core
  - nxp FXOR / CMPL_SUM dropped
  - ambiq undoes HW complement if it has one
- refout != refin: core bit-reverses the result (RBIT)
- HW keeps all per-byte work (poly, width, refin, init)
  - "lowest common denominator" review answer: stm32/s32 gain arbitrary polys; only renesas/ambiq lose, and only what their HW can't do
  - concession if pushed: keep refin/refout separate in crc_cfg, mixed -> -ENOTSUP
- length granularity (ambiq, renesas 32-bit): core feeds aligned part to HW, finishes tail in SW from the raw result
  - needs a generic bitwise SW engine taking crc_cfg
- public model = full Rocksoft params {width, poly, init, refin, refout, xorout}
  - catalogue algorithms become const structs
  - replaces `type`, *_INIT_VAL, *_XOR_OUT

### init domain

- define init as the catalogue (unreflected) value
  - hardware disagrees on the domain -> drivers convert; API stays hardware-independent
- per hardware:
  - silabs: register LSB-first, INIT copied as-is (EFR32xG24 RM §14.3.2, §14.5.5) -> reverse always
    - driver reverses 16-bit seeds but not 32-bit -> inconsistent
  - renesas: seed written to CRCDOR, reflected in LSB-first mode (RA6M1 §36.2.4; Figure 36.2 in §36.3.1 = KERMIT) -> reverse when reflecting
  - stm32: reversal only on input/output; RESET copies CRC_INIT into DR (RM0456 §24.3.3, §24.4.3; RM0351 §15.3.3) -> unreflected, pass through
    - matches rbr backport (FW-2377): crc32 chains with init = RBIT(~prev)
  - sf32lb: same register model as stm32 (UM5201 §12.2.4, §12.2.7) -> pass through
  - nxp kinetis/mcx: seed written to CRC_DATA with WAS (KE1xF RM §31.3.1); does TOT apply to it? manual examples only use seed 0
  - nxp lpc: SEED loads SUM "with a selected bit order and 1's complement pre-processes" (LPC55S6x UM §46.6.2)
    - CMPL_SUM doesn't apply: CRC-32 set-up writes 0xFFFFFFFF with CMPL_SUM on (§46.7.3)
    - bit order: open, set-up examples all symmetric
- subsys reflected functions take the reflected-register value (crc16_reflect seed) -> core converts
- upstream crc32_ieee_update passes ~crc without RBIT -> chaining wrong on stm32 per RM0456 §24.3.3 (not yet run on HW)
  - test: crc32_ieee_update(crc32_ieee(a), b) == crc32_ieee(a||b)
- in-tree driver tests use 0 / all-ones seeds, which can't distinguish the domains
  - add an asymmetric-seed test (e.g. CRC-16/RIELLO)

### subsystem

- CRC_DRIVER_HAS_* shape how the subsystem uses HW, not the driver API
  - build-time per-algorithm HW/SW choice for the plain sys/crc.h names
  - driver API only reports -ENOTSUP
  - optional: move symbols to subsys/crc/Kconfig
- plain names keep current behaviour
- explicit HW path: *_hw functions (RFC presents *_hw and *_sw as equal options)
  - device argument: multiple instances, no zephyr,crc dependency
  - int return, result via out param: errors reach the caller
  - only for algorithms some HW supports
  - thin inline helpers over begin/update/finish, not syscalls
  - room for -EBUSY / timeout later
- length threshold on plain names
  - easier to merge (no public API change)
  - less flexible: caller choice also covers accelerator locking, not just time
- no one-shot syscall for now

### open

- ambiq: does RESULT read back complemented? no manual available -> tests/drivers/crc on apollo510_evb, or ask the driver author
- nxp (both blocks): seed bit order under reflection -> asymmetric-seed test on HW
- renesas ra0e1: per-part capability check in driver
- renesas update(): CRC16_ITU_T falls into the 32-bit branch (upstream bug, separate commit + odd-length test)
- nxp crc_nxp.c: crc32_c loses its xorout under the xor_out WIP
