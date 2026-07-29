// Stock ARM7. No custom code is needed to reach the Wi-Fi slots: readFirmware() and
// writeFirmware() are ARM9-callable wrappers that FIFO to a handler installSystemFIFO()
// installs here, so this side only has to start the FIFO and stay alive.
//
// Unlike SafeNANDManager there is no SD/MMC or I2C work here -- the Wi-Fi settings
// live on the SPI flash chip, not in the NAND.

#include <nds.h>

int main(void)
{
    irqInit();
    fifoInit();
    initClockIRQTimer(3);

    installSystemFIFO();

    irqSet(IRQ_VBLANK, inputGetAndSend);
    irqEnable(IRQ_VBLANK);

    while (1)
        swiWaitForVBlank();

    return 0;
}
