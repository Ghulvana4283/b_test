/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

#ifdef USE_SPI

#include "drivers/bus.h"
#include "drivers/bus_spi.h"
#include "drivers/bus_spi_impl.h"
#include "drivers/exti.h"
#include "drivers/io.h"
#include "drivers/rcc.h"

static SPI_InitTypeDef defaultInit = {
    .SPI_Mode = SPI_Mode_Master,
    .SPI_Direction = SPI_Direction_2Lines_FullDuplex,
    .SPI_DataSize = SPI_DataSize_8b,
    .SPI_NSS = SPI_NSS_Soft,
    .SPI_FirstBit = SPI_FirstBit_MSB,
    .SPI_CRCPolynomial = 7,
    .SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8,
};

void spiInitDevice(SPIDevice device)
{
    spiDevice_t *spi = &(spiDevice[device]);

    // These should go away
#ifdef SDCARD_SPI_INSTANCE
    if (spi->dev == SDCARD_SPI_INSTANCE) {
        spi->leadingEdge = true;
    }
#endif
#ifdef RX_SPI_INSTANCE
    if (spi->dev == RX_SPI_INSTANCE) {
        spi->leadingEdge = true;
    }
#endif

    // Enable SPI clock
    RCC_ClockCmd(spi->rcc, ENABLE);
    RCC_ResetCmd(spi->rcc, ENABLE);

    IOInit(IOGetByTag(spi->sck),  OWNER_SPI_SCK,  RESOURCE_INDEX(device));
    IOInit(IOGetByTag(spi->miso), OWNER_SPI_MISO, RESOURCE_INDEX(device));
    IOInit(IOGetByTag(spi->mosi), OWNER_SPI_MOSI, RESOURCE_INDEX(device));

#if defined(STM32F3) || defined(STM32F4)
    IOConfigGPIOAF(IOGetByTag(spi->sck),  SPI_IO_AF_CFG, spi->af);
    IOConfigGPIOAF(IOGetByTag(spi->miso), SPI_IO_AF_CFG, spi->af);
    IOConfigGPIOAF(IOGetByTag(spi->mosi), SPI_IO_AF_CFG, spi->af);
#endif
#if defined(STM32F10X)
    IOConfigGPIO(IOGetByTag(spi->sck), SPI_IO_AF_SCK_CFG);
    IOConfigGPIO(IOGetByTag(spi->miso), SPI_IO_AF_MISO_CFG);
    IOConfigGPIO(IOGetByTag(spi->mosi), SPI_IO_AF_MOSI_CFG);
#endif

    // Init SPI hardware
    SPI_I2S_DeInit(spi->dev);

    // XXX Everything below will go away

    if (spi->leadingEdge) {
        defaultInit.SPI_CPOL = SPI_CPOL_Low;
        defaultInit.SPI_CPHA = SPI_CPHA_1Edge;
    } else {
        defaultInit.SPI_CPOL = SPI_CPOL_High;
        defaultInit.SPI_CPHA = SPI_CPHA_2Edge;
    }

#ifdef STM32F303xC
    // Configure for 8-bit reads.
    SPI_RxFIFOThresholdConfig(spi->dev, SPI_RxFIFOThreshold_QF);
#endif

    SPI_Init(spi->dev, &defaultInit);
    SPI_Cmd(spi->dev, ENABLE);
}

// return uint8_t value or -1 when failure
uint8_t spiTransferByte(SPI_TypeDef *instance, uint8_t txByte)
{
    uint16_t spiTimeout = 1000;

    while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_TXE) == RESET)
        if ((spiTimeout--) == 0)
            return spiTimeoutUserCallback(instance);

#ifdef STM32F303xC
    SPI_SendData8(instance, txByte);
#else
    SPI_I2S_SendData(instance, txByte);
#endif
    spiTimeout = 1000;
    while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_RXNE) == RESET)
        if ((spiTimeout--) == 0)
            return spiTimeoutUserCallback(instance);

#ifdef STM32F303xC
    return ((uint8_t)SPI_ReceiveData8(instance));
#else
    return ((uint8_t)SPI_I2S_ReceiveData(instance));
#endif
}

/**
 * Return true if the bus is currently in the middle of a transmission.
 */
bool spiIsBusBusy(SPI_TypeDef *instance)
{
#ifdef STM32F303xC
    return SPI_GetTransmissionFIFOStatus(instance) != SPI_TransmissionFIFOStatus_Empty || SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_BSY) == SET;
#else
    return SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_TXE) == RESET || SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_BSY) == SET;
#endif

}

bool spiTransfer(SPI_TypeDef *instance, const uint8_t *txData, uint8_t *rxData, int len)
{
    uint16_t spiTimeout = 1000;

    uint8_t b;
    instance->DR;
    while (len--) {
        b = txData ? *(txData++) : 0xFF;
        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_TXE) == RESET) {
            if ((spiTimeout--) == 0)
                return spiTimeoutUserCallback(instance);
        }
#ifdef STM32F303xC
        SPI_SendData8(instance, b);
#else
        SPI_I2S_SendData(instance, b);
#endif
        spiTimeout = 1000;
        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_RXNE) == RESET) {
            if ((spiTimeout--) == 0)
                return spiTimeoutUserCallback(instance);
        }
#ifdef STM32F303xC
        b = SPI_ReceiveData8(instance);
#else
        b = SPI_I2S_ReceiveData(instance);
#endif
        if (rxData)
            *(rxData++) = b;
    }

    return true;
}

static uint16_t spiDivisorToBRbits(SPI_TypeDef *instance, uint16_t divisor)
{
#if !(defined(STM32F1) || defined(STM32F3))
    // SPI2 and SPI3 are on APB1/AHB1 which PCLK is half that of APB2/AHB2.

    if (instance == SPI2 || instance == SPI3) {
        divisor /= 2; // Safe for divisor == 0 or 1
    }
#else
    UNUSED(instance);
#endif

    return divisor ? ((ffs(divisor | 0x100) - 2) << 3) : 0;
}

static void spiSetDivisorBRreg(SPI_TypeDef *instance, uint16_t divisor)
{
#define BR_BITS ((BIT(5) | BIT(4) | BIT(3)))
    const uint16_t tempRegister = (instance->CR1 & ~BR_BITS);
    instance->CR1 = tempRegister | spiDivisorToBRbits(instance, divisor);
#undef BR_BITS
}

void spiSetDivisor(SPI_TypeDef *instance, uint16_t divisor)
{
    SPI_Cmd(instance, DISABLE);
    spiSetDivisorBRreg(instance, divisor);
    SPI_Cmd(instance, ENABLE);
}

void spiBusSetDivisor(busDevice_t *bus, uint16_t divisor)
{
    spiSetDivisor(bus->busdev_u.spi.instance, divisor);
    bus->busdev_u.spi.modeCache = bus->busdev_u.spi.instance->CR1;
}

void spiBusSetModeAndDivisor(busDevice_t *bus, SPIMode_e mode, SPIClockDivider_e divider)
{
    switch (mode) {
    case SPI_MODE0:
        defaultInit.SPI_CPOL = SPI_CPOL_Low;
        defaultInit.SPI_CPHA = SPI_CPHA_1Edge;
        break;
    case SPI_MODE1:
        defaultInit.SPI_CPOL = SPI_CPOL_Low;
        defaultInit.SPI_CPHA = SPI_CPHA_2Edge;
        break;
    case SPI_MODE2:
        defaultInit.SPI_CPOL = SPI_CPOL_High;
        defaultInit.SPI_CPHA = SPI_CPHA_1Edge;
        break;
    case SPI_MODE3:
        defaultInit.SPI_CPOL = SPI_CPOL_High;
        defaultInit.SPI_CPHA = SPI_CPHA_2Edge;
        break;
    }

    SPI_Init(bus->busdev_u.spi.instance, &defaultInit);
    spiSetDivisorBRreg(bus->busdev_u.spi.instance, divider);
#ifdef STM32F303xC
    // Configure for 8-bit reads.
    SPI_RxFIFOThresholdConfig(bus->busdev_u.spi.instance, SPI_RxFIFOThreshold_QF);
#endif

    bus->busdev_u.spi.modeCache = bus->busdev_u.spi.instance->CR1;
}

void spiBusModeSetup(const busDevice_t *bus)
{
    // XXX We rely on MSTR bit to detect valid modeCache
    if (bus->busdev_u.spi.modeCache) {
        SPI_Cmd(bus->busdev_u.spi.instance, DISABLE);
        // XXX Compare overheads of checking v.s. register writing v.s. transient signal glitches
        if (bus->busdev_u.spi.instance->CR1 != bus->busdev_u.spi.modeCache) {
            bus->busdev_u.spi.instance->CR1 = bus->busdev_u.spi.modeCache;
        }
    }
    SPI_Cmd(bus->busdev_u.spi.instance, ENABLE);
}
#endif
