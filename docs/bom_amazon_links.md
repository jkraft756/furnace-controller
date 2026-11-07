# Bill of Materials (BOM) & Amazon Shopping Guide

This shopping guide lists every component needed to build, bench-test, and permanently mount the **Heil `G96CTN0601714A1` & Wood Boiler Parallel ABCD Bus Co-Controller** around the **AITRIP `ESP32-S3-DevKitC-1-N16R8`** (`44-Pin` / `22x2`, `25.4mm` row spacing).

---

## Part 1: Core Electronics & Sensors

| # | Component | Exact Spec to Verify on Amazon Page | Amazon Link |
| :-: | :--- | :--- | :--- |
| **1** | **3.3V RS-485 to TTL Transceiver Module** | Verify it states **`3.3V` compatible (`MAX3485` / `SP3485`)** with screw terminals for `A` & `B`. *(Either the 4-pin Auto-Direction `VCC/GND/TXD/RXD` board or the 6-pin `RO/DI/DE/RE` board works—see Section 3 below).* | [Search: 3.3V MAX3485 RS485 to TTL Module](https://www.amazon.com/s?k=MAX3485+3.3V+RS485+to+TTL+module) |
| **2** | **24VAC to 5VDC Buck Converter** | Must state **`AC/DC` Input (`15V–28V AC` / `24VAC`)** to **`5V DC` (`2A` or `3A`)** output. *(Do not buy a DC-only buck converter, as the furnace `C` & `D` terminals supply 24V **AC**).* | [Search: 24VAC to 5VDC Buck Converter](https://www.amazon.com/s?k=24VAC+to+5VDC+converter+step+down) |
| **3** | **Waterproof `DS18B20` Temperature Probes + Screw-Terminal Pull-Up Adapter** | Choose a **2-Pack (`2m` / `6.5ft` or `3m` / `10ft` stainless steel probes)** that **includes the pluggable 3-pin screw-terminal adapter board** (which has the required `4.7 kΩ` pull-up resistor built right onto the board). | [Search: DS18B20 Waterproof Probe + Terminal Adapter Kit](https://www.amazon.com/s?k=DS18B20+waterproof+temperature+sensor+terminal+adapter+module) |
| **4** | **Inline Fuse Holder + `1A` / `2A` Fuse (24VAC Protection)** | `16 AWG` or `18 AWG` inline fuse holder with `1A` or `2A` fuses to protect the Heil furnace's internal `3A/5A` control board transformer fuse. | [Search: Inline Fuse Holder with 1A/2A Fuses](https://www.amazon.com/s?k=inline+fuse+holder+1A+2A+fuse+assortment) |

---

## Part 2: Breakout Board, Breadboards, Enclosure & Mounting

| # | Component | Exact Spec to Verify on Amazon Page | Amazon Link |
| :-: | :--- | :--- | :--- |
| **5** | **44-Pin ESP32-S3 Screw-Terminal Breakout Board** | Verify the title/photos say **"44-Pin (`22x2` pins)"** for **`ESP32-S3-DevKitC-1`** with **`1.0"` (`25.4mm`) pin row spacing**. *(Do not buy a 30-pin or 38-pin ESP32-WROOM breakout).* | [Search: 44-Pin ESP32-S3-DevKitC-1 Breakout Board](https://www.amazon.com/s?k=ESP32-S3-DevKitC-1+44+pin+breakout+board+screw+terminal) |
| **6** | **Desk Prototyping Breadboards + DuPont Jumpers** | **830-Point Solderless Breadboards (2-Pack or 3-Pack) + Male/Female DuPont Jumper Wires**. Snapping 2 breadboards side-by-side gives open tie-points on both sides of the wide ESP32-S3. | [Search: 830 Breadboard Kit + DuPont Jumper Wires](https://www.amazon.com/s?k=830+point+breadboard+jumper+wires+kit) |
| **7** | **Clear-Lid Plastic (ABS) Hinged Enclosure (`~6"x6"x3.5"` or `8"x6"x4"`)** | Choose **ABS/Polycarbonate (Non-Metallic)** so Wi-Fi passes through (`-30 dBm`), with a **Clear Hinged Cover** (to see the RGB status LED) and an **internal mounting backplate**. | [Search: QILIPSU / Zulkit Clear Lid Hinged ABS Junction Box](https://www.amazon.com/s?k=clear+cover+hinged+junction+box+ABS+mounting+plate+6x6) |
| **8** | **Rubber-Coated Neodymium Mounting Magnets** | Screw 4 of these onto the back of the plastic enclosure so the entire controller box sticks firmly to the side of the steel Heil furnace cabinet **with zero drilling**. | [Search: Rubber Coated Mounting Magnets with M4/M5 Studs](https://www.amazon.com/s?k=rubber+coated+mounting+magnets+female+thread+M4) |
| **9** | **Wago 221 Lever-Nut Assortment (or 5-Port 221-415)** | Makes splitting `3V3` and `GND` between the ESP32-S3, RS-485 transceiver, and `DS18B20` adapter board 100% tool-free and rock-solid. | [Search: Wago 221 Lever Nuts](https://www.amazon.com/s?k=Wago+221+lever+nuts+5+conductor) |
| **10** | **`18/4` Solid Thermostat Cable** | `18 AWG` 4-conductor (`Red, White, Green, Blue/Yellow`) cable to run from the furnace's `A, B, C, D` terminal block into the controller enclosure. | [Search: 18/4 Thermostat Wire](https://www.amazon.com/s?k=18%2F4+thermostat+wire+25+ft) |
| **11** | **M2.5 & M3 Nylon Hex Standoff Assortment Kit** | Includes both **`M3`** (fits the `3.0mm–3.2mm` mounting holes on the 44-pin ESP32-S3 breakout board) and **`M2.5`** (fits the smaller `2.5mm` holes on the RS-485 transceiver and `DS18B20` terminal adapter). | [Search: M2.5 & M3 Nylon Hex Standoff Assortment Kit](https://www.amazon.com/s?k=M2.5+M3+nylon+hex+standoff+spacer+screw+nut+assortment+kit) |
| **12** | **Adhesive PCB Snap-In Standoffs (No-Drill Option)** | Peel-and-stick 3M backing lets you snap the smaller RS-485 module, `DS18B20` pull-up adapter, and buck converter onto the enclosure backplate **without drilling or threading tiny nuts**. | [Search: Adhesive PCB Snap-In Standoffs](https://www.amazon.com/s?k=adhesive+pcb+standoff+spacer+support+clips) |

---

## Part 3: RS-485 Transceiver Module Wiring Note (4-Pin vs. 6-Pin)

When shopping for Item #1 (`3.3V RS-485 to TTL Module`), you will encounter two common styles on Amazon. **Both work seamlessly with our ESP32-S3 firmware**:

### Option A: 4-Pin "Auto-Direction" 3.3V RS-485 Board (`VCC`, `GND`, `TXD`, `RXD`)
- Includes an onboard auto-direction circuit (`74HC04` / transistor driver) that automatically switches the RS-485 chip into transmit mode only while bits are actively shifting out of `TXD`.
- **Wiring to ESP32-S3**:
  - `VCC` $\rightarrow$ `3V3`
  - `GND` $\rightarrow$ `GND`
  - `RXD` (or `RO`) $\rightarrow$ **`GPIO 18`** (`UART1 RX`)
  - `TXD` (or `DI`) $\rightarrow$ **`GPIO 17`** (`UART1 TX`)
  - *(Leave `GPIO 21` unconnected)*

### Option B: 6-Pin "Manual Direction" `MAX3485` Board (`VCC`, `GND`, `RO`, `DI`, `DE`, `RE`)
- Exposes explicit Driver Enable (`DE`) and Receiver Enable (`RE`) pins.
- **Wiring to ESP32-S3**:
  - `VCC` $\rightarrow$ `3V3`
  - `GND` $\rightarrow$ `GND`
  - `RO` $\rightarrow$ **`GPIO 18`** (`UART1 RX`)
  - `DI` $\rightarrow$ **`GPIO 17`** (`UART1 TX`)
  - Jumper `DE` and `RE` together $\rightarrow$ **`GPIO 21`** (`UART1 RTS / DE_RE`)
