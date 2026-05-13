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
