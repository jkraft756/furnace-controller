# ABCD RS-485 Protocol & Ion Thermostat (`0x2001`) Findings

**Date:** 2026-11-13  
**Hardware:** Heil `G96CTN0601714A1` Furnace (`0x4401`) + Carrier/Heil Ion `INFINITY NEXT GEN UI` Thermostat (`0x2001`, SW `CESR131626-04.05SYST0101CW`, S/N `4321W205021`) + ESP32-S3 Co-Controller (`0x9201`)

---

## 1. Physical Layer & Frame Structure

- **Bus:** RS-485 half-duplex, `38400` baud, `8N1`
- **Frame Format:**
  ```text
  [DestHi][DestLo][SrcHi][SrcLo][Len][0x00][0x00][Func][T0][T1][T2][...Payload...][CrcLo][CrcHi]
  ```
  - `Total Frame Length = Len + 10` bytes (`Len` = 3-byte table ID + payload bytes).
  - `ABCD_MAX_FRAME_BYTES` was increased from `160` to `265` (in `src/abcd_bus.h`) so full-length responses such as `00:3B:03` (`163` bytes total) decode without truncation.
- **Checksum:** CRC-16/ARC (poly `0x8005` reflected `0xA001`, init `0x0000`), stored little-endian (`crcLo`, `crcHi`).
- **Known Bus Addresses:**
  - `0x2001` — Ion Thermostat (`INFINITY NEXT GEN UI`, Bus Master)
  - `0x4401` — Heil `G96CTN0601714A1` Furnace
  - `0x9201` — ESP32 Co-Controller / SAM (System Access Module)
  - `0xF1F1` — Broadcast
- **Function Opcodes:**
  - `0x06` — `ACK/RESP`
  - `0x0B` — `FUNC_READ`
  - `0x0C` — `FUNC_WRITE`
  - `0x15` — `NAK` (observed error codes: `0x04` = invalid/unknown table, `0x0A` = read/write prohibited or unsupported)

---

## 2. Ion Thermostat (`0x2001`) Table Map (`0x9201` → `0x2001` Live Probes)

Carrier/Heil `INFINITY NEXT GEN UI` uses `00:XX:01` as an ASCII descriptor for each table group, and `00:XX:02+` for data sub-tables.

### Device & Time Tables
| Table | Result | Payload | Notes / Decoded Fields |
|---|---|---|---|
| `00:01:01` | `ACK/RESP` | 25B | Descriptor `"DEVCONFG"` |
| `00:01:02`–`03` | `NAK (0x0A)` | 1B | Prohibited / unsupported |
| `00:01:04` | `ACK/RESP` | 120B | Device ID: `"INFINITY NEXT GEN UI"`, `"CESR131626-04.05SYST0101CW"`, S/N `"4321W205021"` |
| `00:01:05`–`06` | `NAK (0x0A)` | 1B | Prohibited / unsupported |
| `00:02:02` | `ACK/RESP` | 3B | Live clock: `12 23 05` → `18:35`, Day of Week `5` |

### SAM / Zone Interface (`00:3B:xx`) & Unimplemented Ranges (`00:38`–`00:3F`)
| Table | Result | Payload | Notes / Decoded Fields |
|---|---|---|---|
| `00:38:01`–`00:3A:01` | `NAK (0x04)` | 1B | Invalid / unsupported |
| `00:3B:01` | `ACK/RESP` | 25B | Descriptor `"SAMINFO"` (SAM interface info, not Zone 1 config) |
| `00:3B:02` | `ACK/RESP` | 29B | All `0x00` (unpopulated SAM slot) |
| `00:3B:03` | `ACK/RESP` | 150B | All `0x00` (`163` bytes total on wire) |
| `00:3B:04` | `ACK/RESP` | 11B | All `0x00` |
| `00:3B:05` | `ACK/RESP` | 11B | All `0x00` |
| `00:3C:01`–`03` | `NAK (0x04)` | 1B | Invalid / unsupported |
| `00:3D:01`–`04`, `06` | `NAK (0x04)` | 1B | Invalid / unsupported (`00:3D:05` no reply in 60 ms window) |
| `00:3E:01`–`06` | `NAK (0x04)` | 1B | Invalid / unsupported |
| `00:3F:01`–`02`, `04`–`06` | `NAK (0x04)` | 1B | Invalid / unsupported (`00:3F:03` no reply in 60 ms window) |

### Schedule, Temperature & Misc Tables (`00:40`–`00:4A`)
| Table | Result | Payload | Notes / Decoded Fields |
|---|---|---|---|
| `00:40:01` | `ACK/RESP` | 59B | Descriptor `"SCHEDULE"` |
| `00:40:02`–`06` | `ACK/RESP` | 70B each | 7× 10-byte daily schedule slots (e.g., `18 00 20 01 44 00 58 02 58 02`; `0x44` = `68°F` Heat SP, `0x58` = `88°F` Cool SP) |
| `00:41:01` | `ACK/RESP` | 17B | Descriptor `"TEMP"` |
| `00:41:02` | `ACK/RESP` | 93B | Live sensed temps (BE float32): bytes `9..12` = `42 91 33 72` (`72.60°F` room temp) |
| `00:41:03`–`06` | `NAK (0x0A)` | 1B | Prohibited / unsupported |
| `00:42:01` | `ACK/RESP` | 21B | Descriptor `"LASTTEN"` |
| `00:42:02`–`03` | `ACK/RESP` | 72B / 24B | Historical / last-ten records |
| `00:43:01`–`00:45:01` | `NAK (0x04)` | 1B | Invalid / unsupported |
| `00:46:01` | `ACK/RESP` | 45B | Descriptor `"MISC1"` |
| `00:46:02`–`03` | `ACK/RESP` | 16B / 27B | `00:46:02`: `02 00 00 00 00 0F ... 0D 03 20` |
| `00:47:01`–`02` | `ACK/RESP` | 17B / 1B | Descriptor `"RESTMR"`, `:02` = `00` (`:03` = `NAK 0x0A`) |
| `00:48:01`–`02` | `ACK/RESP` | 17B / 13B | Descriptor `"HEALTH"`, `:02` = `01 01 00 00 06 E4 05 B6 00 00 00 39 00` (`:03` = `NAK 0x0A`) |
| `00:4A:01`–`03` | `ACK/RESP` | 21B / 152B / 26B | Descriptor `"MISC2"`, `:02` all `0x00`, `:03` = `01` + `0x00`s |

### `SYSCTRL` Group (`00:49:01`–`00:49:0B`, 11 Sub-Tables)
| Table | Result | Payload | Notes / Decoded Fields |
|---|---|---|---|
| `00:49:01` | `ACK/RESP` | 35B | Descriptor `"SYSCTRL"` (byte 11 = `0x0B` = 11 sub-tables) |
| `00:49:02` | `ACK/RESP` | 96B | All `0x00` |
| `00:49:03` | `ACK/RESP` | 96B | **Configured Zone 1–8 Setpoints** (BE float32): bytes `0..3` = Zone 1 Heat SP (`42 82 00 00` = `65.0°F`), Zones 2–8 = `50.0°F`; bytes `64..67` = Zone 1 Cool SP (`42 A8 00 00` = `84.0°F`), Zones 2–8 = `90.0°F`. **Read-only from `0x9201` (`FUNC_WRITE` returns `NAK 0x0A`).** |
| `00:49:04` | `ACK/RESP` | 41B | Bytes `0..3` rolling/time-varying value (`C7 AD 06 D2` → `C7 A6 F0 0E`); bytes `37..40` = `42 82 00 00` (`65.0°F`) |
| `00:49:05` | `ACK/RESP` | 88B | All `0x00` |
| `00:49:06` | `ACK/RESP` | 136B | Contains BE float32 values `~11600.0`, `15.0`, `58000.0` |
| `00:49:07` | `ACK/RESP` | 92B | Contains `0.85` (`3F 59 99 99`) and `0.70` (`3F 33 33 33`) |
| `00:49:08` | `ACK/RESP` | 3B | `00 00 00` |
| `00:49:09` | `ACK/RESP` | 100B | All `0x00` |
| `00:49:0A` | `ACK/RESP` | 70B | **Active/Effective Setpoints**: bytes `0..2` = `00 00 04`; bytes `3..34` = Cool SPs (Z1 `84.0°F`); bytes `35..66` = Heat SPs (Z1 `42 83 80 00` = `65.75°F`); bytes `67..69` = `00 00 00` |
| `00:49:0B` | `ACK/RESP` | 96B | Contains `58000.0` at bytes `64..67` |
| `00:49:0C` | `NAK (0x0A)` | 1B | End of `SYSCTRL` group |

---

## 3. Furnace (`0x4401`) & Broadcast Tables (Observed in Firmware)

- **`00:03:05` (`FUNC_WRITE` `0x2001` → `0x4401`):** Master Thermostat-to-Furnace Heat Stage & Blower CFM Command (12-byte payload).
  - Byte `0`: `heatStageCmd` (`0x00` = Heat OFF, `0x02` = Stage 1 Low 39k, `>0x02` = Stage 2 High 60k)
  - Bytes `4..5`: `cmdCfm` (big-endian uint16; observed `0x0000` = `AUTO`, `0x019D` = `413 CFM` `LOW`, `0x029B` = `667 CFM` `MED`, `0x041A` = `1050 CFM` `HIGH`)
  - Example (`Fan=MED`): `44 01 20 01 0F 00 00 0C 00 03 05 00 00 00 00 02 9B 00 00 00 00 00 78 3A 9B`
- **`00:03:06` (`0x4401`):** Furnace airflow, torque/flow, target CFM, static pressure (`0.01 in.wc`).
- **`00:03:16` (`0x4401`):** ECM motor active stage, target CFM, live RPM.
- **`00:04:02` (`0x4401`):** Heil ECM profile (`33B`).
- **`00:04:04` (`0x4401`):** Relay/ignition mask (`0x06` IDLE, `0x0E` POST-PURGE, `0x1E` INDUCER+HSI, `0x1F` FLAME ON), target CFM, outFlow.
- **`00:04:0D` (`0x4401`):** Gas burner / ignition stage status.
- **`00:04:13` (`0x4401`):** ECM motor power (IEEE-754 Watts).
- **`00:04:14` (`0x2001` Broadcast):** Room temperature in `1/16 °F` (`payload[1..2]`).

---

## 4. Control & Contention Findings (2026-11-13)

1. **Why `00:03:05` Spoofing Causes Blower Cycling / Speed Flipping:**
   - The Ion Thermostat (`0x2001`) remains the physical bus master and periodically transmits `00:03:05` with its own `cmdCfm` (e.g., `0 CFM` / `AUTO`).
   - `FurnaceController::loop()` (`src/controller.cpp`) reactively re-asserts its own spoofed `00:03:05` frame (`src_addr = 0x2001`) after `45 ms` of bus idle, creating a tug-of-war where the furnace ECM motor briefly ramps down on the thermostat's `0 CFM` command and ramps back up on the ESP32's override.
2. **`FUNC_WRITE` on `00:49:03` (`SYSCTRL`) Rejected:**
   - Sending `FUNC_WRITE` from `0x9201` → `0x2001` to change Zone 1 Heat Setpoint (`65.0°F` → `64.0°F`) returned **`NAK (Error 0x0A)`**, and readback confirmed the setpoint remained `65.0°F`.
3. **Fan Mode (`AUTO` → `MED`) Diff:**
   - Switching Fan `AUTO` → `MED` on the Ion Thermostat wall screen immediately updated `00:03:05` (`0 CFM` → `667 CFM`), while `00:49:03`, `00:49:04` (bytes `4..40`), `00:49:05`, `00:49:08`, `00:49:0A`, `00:46:02`, `00:46:03`, and `00:3B:02` showed no Fan Mode change.

---

## 5. Representative Raw Hex Captures

- **`00:01:04` (`INFINITY NEXT GEN UI` Identity):**
  `92 01 20 01 7B 00 00 06 00 01 04 49 4E 46 49 4E 49 54 59 20 4E 45 58 54 20 47 45 4E 20 55 49 20 20 20 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 43 45 53 52 31 33 31 36 32 36 2D 30 34 2E 30 35 53 59 53 54 30 31 30 31 43 57 00 00 00 00 00 00 00 00 00 00 34 33 32 31 57 32 30 35 30 32 31 00 34 33 32 31 57 32 30 35 30 32 31 00 00 00 00 00 00 00 00 00 00 00 00 00 AE 63`
- **`00:3B:01` (`SAMINFO`):**
  `92 01 20 01 1C 00 00 06 00 3B 01 00 20 53 41 4D 49 4E 46 4F 00 01 16 06 19 01 1D 03 96 03 0B 03 0B 03 34 03 E7 3F`
- **`00:49:03` (`SYSCTRL` Setpoints, Zone 1 Heat `65.0°F` / Cool `84.0°F`):**
  `92 01 20 01 63 00 00 06 00 49 03 42 82 00 00 42 48 00 00 42 48 00 00 42 48 00 00 42 48 00 00 42 48 00 00 42 48 00 00 42 48 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 42 A8 00 00 42 B4 00 00 42 B4 00 00 42 B4 00 00 42 B4 00 00 42 B4 00 00 42 B4 00 00 42 B4 00 00 F6 7E`
- **`00:49:03` Write Rejection (`NAK 0x0A`):**
  `92 01 20 01 01 00 00 15 0A E7 EE`
