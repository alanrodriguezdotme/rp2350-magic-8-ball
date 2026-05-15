#pragma once

#include <stdbool.h>

// Read the BOOTSEL button as a regular GPIO. Briefly suspends XIP
// (~10 µs) so DON'T call this while flash-resident interrupts (DMA
// refills, USB, lvgl callbacks living in flash) are time-critical.
// Safe between WAV plays and during quiet idle ticks.
//
// Pattern lifted from pico-examples/picoboard/button/button.c.
bool boot_button_pressed(void);

// Edge-triggered: returns true exactly once per press transition. Use
// from the main loop to drive the volume-cycle action.
bool boot_button_just_pressed(void);
