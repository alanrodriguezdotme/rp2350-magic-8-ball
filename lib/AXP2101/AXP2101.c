#include "AXP2101.h"

#include <stdio.h>

// Register map (subset). Names follow the AXP2101 datasheet / XPowersLib
// convention.
#define AXP2101_REG_CHIP_ID (0x03)

#define AXP2101_REG_INT_EN0 (0x40)
#define AXP2101_REG_INT_EN1 (0x41)
#define AXP2101_REG_INT_EN2 (0x42)

#define AXP2101_REG_INT_STS0 (0x48)
#define AXP2101_REG_INT_STS1 (0x49)
#define AXP2101_REG_INT_STS2 (0x4A)

// In INT_EN1 / INT_STS1 (0x41 / 0x49):
//   bit 0 : PKEY long-press
//   bit 1 : PKEY short-press
//   bit 2 : PKEY negative edge (released)
//   bit 3 : PKEY positive edge (pressed)
#define AXP2101_PKEY_SHORT_BIT (1u << 1)

// On-chip fuel gauge state-of-charge register (datasheet: BAT_PERCENTAGE_DATA).
#define AXP2101_REG_BAT_PERCENT (0xA4)

bool AXP2101_Init(void)
{
    uint8_t chip_id = DEV_I2C_Read_Byte(AXP2101_I2C_ADDR, AXP2101_REG_CHIP_ID);
    printf("AXP2101 chip id: 0x%02x\r\n", chip_id);

    // Datasheet says reg 0x03 reads back a fixed pattern; observed
    // values across AXP2101 silicon vary but are non-zero and non-0xFF.
    // Reject only the obvious "no chip on bus" cases.
    if (chip_id == 0x00 || chip_id == 0xFF)
    {
        return false;
    }

    // Mask everything in EN0 / EN2; in EN1 enable only PKEY short-press
    // so the status latch we poll only ever reflects a real button press.
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_EN0, 0x00);
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_EN1, AXP2101_PKEY_SHORT_BIT);
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_EN2, 0x00);

    // Clear any IRQ status bits that latched during boot (write-1-to-clear).
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_STS0, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_STS1, 0xFF);
    DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_STS2, 0xFF);

    return true;
}

bool AXP2101_PwrKey_Short_Pressed(void)
{
    uint8_t sts = DEV_I2C_Read_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_STS1);
    if (sts & AXP2101_PKEY_SHORT_BIT)
    {
        // Write-1-to-clear just the bit we consumed; leave any other
        // bits alone so future enablements don't lose events.
        DEV_I2C_Write_Byte(AXP2101_I2C_ADDR, AXP2101_REG_INT_STS1, AXP2101_PKEY_SHORT_BIT);
        return true;
    }
    return false;
}

uint8_t AXP2101_GetBatteryPercent(void)
{
    uint8_t v = DEV_I2C_Read_Byte(AXP2101_I2C_ADDR, AXP2101_REG_BAT_PERCENT);
    // Register reads 0xFF on an unpopulated battery; valid SOC is 0..100.
    if (v > 100)
    {
        return 0xFF;
    }
    return v;
}
