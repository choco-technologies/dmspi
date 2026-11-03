#ifndef ESP32S3_SPI_DMA_H
#define ESP32S3_SPI_DMA_H

#include <stddef.h>
#include <stdint.h>

/* Transfers of a few dozen bytes or more by the GDMA through dmdma (esp32s3_spi_dma.c) */
#define ESP32S3_SPI_DMA_MIN_BYTES       64U

/**
 * @brief One full-duplex transfer by DMA (tx NULL: 0xFF is sent, rx NULL: the bytes are dropped)
 * @param index the controller, 0: GP-SPI2, 1: GP-SPI3
 * @param baudrate the clock the controller runs at (for the longest time the transfer can take)
 * @return 0 done, 1 not done and the bus not touched (no dmdma, no streams, no bounce buffers): the caller moves the bytes by itself,
 *         -1 the transfer failed
 */
int esp32s3_spi_dma_transfer(unsigned index, const uint8_t *tx, uint8_t *rx, size_t size, uint32_t baudrate);

/** Gives the streams and the buffers back (deinit of the instance) */
void esp32s3_spi_dma_release(unsigned index);

#endif /* ESP32S3_SPI_DMA_H */
