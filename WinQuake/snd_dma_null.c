/* Null DMA driver used by headless builds; the Quake mixer remains enabled. */
#include "quakedef.h"

static dma_t null_dma;
static byte null_buffer[32768];

qboolean SNDDMA_Init(void)
{
    memset(&null_dma, 0, sizeof(null_dma));
    null_dma.samplebits = 16;
    null_dma.speed = 22050;
    null_dma.channels = 2;
    null_dma.samples = sizeof(null_buffer) / (null_dma.samplebits / 8);
    null_dma.submission_chunk = 1;
    null_dma.soundalive = true;
    null_dma.gamealive = true;
    null_dma.buffer = null_buffer;
    shm = &null_dma;
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    return 0;
}

void SNDDMA_Shutdown(void)
{
    null_dma.gamealive = false;
    null_dma.soundalive = false;
    null_dma.buffer = NULL;
    if (shm == &null_dma)
        shm = NULL;
}

void SNDDMA_Submit(void)
{
}
