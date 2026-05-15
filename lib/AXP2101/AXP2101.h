// Minimal AXP2101 PMIC driver. Only what the sleep manager needs:
//   - confirm the chip is alive on I2C
//   - configure the IRQ mask so PWRKEY short-press is the only event
//     that latches in the IRQ status registers
//   - poll/clear the PWRKEY short-press flag from the main loop
//
// The AXP2101 sits on the shared I2C1 bus at addr 0x34. Hold-to-power-off
// (~10 s) is implemented entirely in PMIC hardware and is unaffected by
// anything we do here.

#ifndef _AXP2101_H_
#define _AXP2101_H_

#include "DEV_Config.h"
#include <stdbool.h>
#include <stdint.h>

#define AXP2101_I2C_ADDR (0x34)

// Probe the chip ID register, mask all IRQs except PWRKEY short-press,
// and clear any pending IRQ flags. Returns true if the chip ID looks
// plausible.
bool AXP2101_Init(void);

// Edge-triggered: returns true exactly once per PWRKEY short-press,
// even if called many times. Reads + write-back-to-clear the IRQ
// status latch in one call. Safe to call every main-loop iteration.
bool AXP2101_PwrKey_Short_Pressed(void);

// State of charge from the on-chip fuel gauge, 0..100. Returns 0xFF if
// the gauge reports an out-of-range value (typically "no battery" or
// "gauge not yet converged"). The AXP2101 runs the gauge automatically
// once a battery is connected; no extra init needed.
uint8_t AXP2101_GetBatteryPercent(void);

#endif // _AXP2101_H_
