/*
 * Simple ARM debug interface for Arduino, using the SWD (Serial Wire Debug) port.
 * Extensions for Freescale Kinetis chips.
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <Arduino.h>
#include "arm_kinetis_debug.h"
#include "arm_kinetis_reg.h"


bool ARMKinetisDebug::startup()
{
    return detect() && resetHalt() && peripheralInit();
}

bool ARMKinetisDebug::detect()
{
    // Make sure we're on a compatible chip. The MDM-AP peripheral is Freescale-specific.
    uint32_t idr;
    if (!apRead(REG_MDM_IDR, idr))
        return false;
    if (idr != 0x001C0000) {
        log(LOG_ERROR, "ARMKinetisDebug: Didn't find a supported MDM-AP peripheral");
        return false;
    }

    return true;
}

bool ARMKinetisDebug::resetHalt()
{
    // System resets can be slow, give them more time than the default.
    const unsigned resetRetries = 2000;

    // Put the control register in a known state, and make sure we aren't already in the middle of a reset
    uint32_t status;
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_CORE_HOLD_RESET))
        return false;
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_SYS_NRESET, -1, resetRetries))
        return false;

    // System reset
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_SYS_RESET_REQ))
        return false;
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_SYS_NRESET, 0))
        return false;
    if (!apWrite(REG_MDM_CONTROL, 0))
        return false;

    // Wait until the flash controller is ready & system is out of reset.
    // Also wait for security bit to be cleared. Early in reset, the chip is determining
    // its security status. When the security bit is set, AHB-AP is disabled.
    if (!apReadPoll(REG_MDM_STATUS, status,
            REG_MDM_STATUS_SYS_NRESET | REG_MDM_STATUS_FLASH_READY | REG_MDM_STATUS_SYS_SECURITY,
            REG_MDM_STATUS_SYS_NRESET | REG_MDM_STATUS_FLASH_READY,
            resetRetries))
        return false;

    // Set up CSW, no auto-increment.
    if (!apWrite(MEM_CSW, CSW_DBGSWENABLE | CSW_MASTER_DEBUG | CSW_HPROT | CSW_32BIT | CSW_ADDRINC_OFF))
        return false;

    // Point at the debug halt control/status register
    if (!apWrite(MEM_TAR, REG_SCB_DHCSR))
        return false;

    /*
     * Enable debug, request a halt, and read back status.
     *
     * This part is somewhat timing critical, since we're racing against the watchdog
     * timer. Avoid memWait() by calling the lower-level interface directly.
     *
     * Since this is expected to fail a bunch before succeeding, mute errors temporarily.
     */

    unsigned haltRetries = 200;
    LogLevel savedLogLevel;
    uint32_t dhcsr;

    setLogLevel(LOG_NONE, savedLogLevel);

    while (haltRetries) {
        haltRetries--;

        if (!apWrite(MEM_DRW, 0xA05F0003))
            continue;
        if (!apRead(MEM_DRW, dhcsr))
            continue;

        if (dhcsr & (1 << 17)) {
            // Halted!
            break;
        }
    }

    // Restore previous settings
    initMemPort();
    setLogLevel(savedLogLevel);

    if (haltRetries) {
        log(LOG_NORMAL, "CPU reset & halt successful. Now in debug mode.");
        return true;
    }

    log(LOG_ERROR, "ARMKinetisDebug: Failed to put CPU in debug halt state. (DHCSR: %08x)", dhcsr);
    return false;
}

bool ARMKinetisDebug::peripheralInit()
{
    // Enable peripheral clocks
    if (!memStore(REG_SIM_SCGC5, 0x00043F82))
        return false;
    if (!memStore(REG_SIM_SCGC6, REG_SIM_SCGC6_FTM0 | REG_SIM_SCGC6_FTM1 | REG_SIM_SCGC6_FTFL))
        return false;

    // Test AHB-AP: Can we successfully write to RAM?
    if (!memStoreAndVerify(0x20000000, 0x31415927))
        return false;
    if (!memStoreAndVerify(0x20000000, 0x76543210))
        return false;

    return true;
}

bool ARMKinetisDebug::flashMassErase()
{
    // Erase all flash, even if some of it is protected.

    uint32_t status;
    if (!apRead(REG_MDM_STATUS, status))
        return false;
    if (!(status & REG_MDM_STATUS_FLASH_READY)) {
        log(LOG_ERROR, "FLASH: Flash controller not ready before mass erase");
        return false;
    }
    if ((status & REG_MDM_STATUS_FLASH_ERASE_ACK)) {
        log(LOG_ERROR, "FLASH: Mass erase already in progress");
        return false;
    }
    if (!(status & REG_MDM_STATUS_MASS_ERASE_ENABLE)) {
        log(LOG_ERROR, "FLASH: Mass erase is disabled!");
        return false;
    }

    log(LOG_NORMAL, "FLASH: Beginning mass erase operation");
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_CORE_HOLD_RESET | REG_MDM_CONTROL_MASS_ERASE))
        return false;

    // Wait for the mass erase to begin (ACK bit set)
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_FLASH_ERASE_ACK, -1)) {
        log(LOG_ERROR, "FLASH: Timed out waiting for mass erase to begin");
        return false;
    }

    // Wait for it to complete (CONTROL bit cleared)
    uint32_t control;
    if (!apReadPoll(REG_MDM_CONTROL, control, REG_MDM_CONTROL_MASS_ERASE, 0, 10000)) {
        log(LOG_ERROR, "FLASH: Timed out waiting for mass erase to complete");
        return false;
    }

    // Check status again
    if (!apRead(REG_MDM_STATUS, status))
        return false;
    if (!(status & REG_MDM_STATUS_FLASH_READY)) {
        log(LOG_ERROR, "FLASH: Flash controller not ready after mass erase");
        return false;
    }

    log(LOG_NORMAL, "FLASH: Mass erase complete");
    return true;
}