#include "boot_button.h"

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"

// On RP2350 the BOOTSEL button is wired to the QSPI chip-select pin.
// Reading it requires floating CS for a few microseconds, which means
// no flash access during that window. Hence the function lives in SRAM
// and disables interrupts (most IRQ handlers run from flash too).
bool __no_inline_not_in_flash_func(boot_button_pressed)(void)
{
    const uint CS_PIN_INDEX = 1;

    uint32_t flags = save_and_disable_interrupts();

    // Force CS to high-impedance so the external pull-down (button to
    // ground) can drive the line.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Settle. Can't sleep_us here — that lives in flash.
    for (volatile int i = 0; i < 1000; ++i)
        ;

    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);

    // Restore CS to normal output mode so XIP works again.
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

bool boot_button_just_pressed(void)
{
    static bool s_was_pressed = false;
    bool now = boot_button_pressed();
    bool edge = now && !s_was_pressed;
    s_was_pressed = now;
    return edge;
}
