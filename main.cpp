#include <chrono>
#include <iostream>
#include <thread>
#include <fstream>

#include <Windows.h>

#include "serrno.h"
#include "types.h"

extern "C" {
#include <ssys/ssys.h>
#include <soundio/soundio.h>
#include <splayer/splayer.h>
}

constexpr int DMA_SIZE_SAMPLES      = 512;
constexpr int DMA_SIZE_SAMPLES_HALF = DMA_SIZE_SAMPLES / 2;

size_t            gDmaProgress              = 0;
s16               gDmaBuf[DMA_SIZE_SAMPLES] = {};
SoundIoOutStream *gStream;
std::ofstream     gRawOut;

void exitCb() {
    splrDeinit();
}

void dmaHalfCplt() {
    splrUpdateTrackData();
    splrSynthesize();
    memcpy(gDmaBuf, gSplrFrontBuf, sizeof(s16) * DMA_SIZE_SAMPLES_HALF);
}

void dmaCplt() {
    splrUpdateTrackData();
    splrSynthesize();
    memcpy(gDmaBuf + DMA_SIZE_SAMPLES_HALF, gSplrFrontBuf, sizeof(s16) * DMA_SIZE_SAMPLES_HALF);
}

void writeCbDummy(SoundIoOutStream *pStream, int pFramesMin, int pFramesMax) {
    return;
}

void writeCb() {
    auto framesLeft = DMA_SIZE_SAMPLES_HALF;
    auto layout     = gStream->layout;

    SoundIoChannelArea *areas;
    SoundIoError        ret;

    while (framesLeft > 0)
    {
        int frameCount = framesLeft;
        ret            = (SoundIoError) soundio_outstream_begin_write(gStream, &areas, &frameCount);
        if (ret != SoundIoErrorNone)
        {
            return;
        }

        for (int i = 0; i < frameCount; ++i)
        {
            for (int j = 0; j < layout.channel_count; ++j)
            {
                auto [ptr, step] = areas[j];
                auto dat         = (s16 *) (ptr + step * i);

                *dat = gDmaBuf[gDmaProgress++];

                if (j == 0)
                {
                    fwrite(dat, 1, 2, gSplrWavOutFile);
                }

                if (gDmaProgress == DMA_SIZE_SAMPLES_HALF)
                {
                    dmaHalfCplt();
                }

                if (gDmaProgress == DMA_SIZE_SAMPLES)
                {
                    dmaCplt();
                    gDmaProgress = 0;
                }
            }
        }

        ret = (SoundIoError) soundio_outstream_end_write(gStream);
        if (ret != SoundIoErrorNone)
        {
            return;
        }

        framesLeft -= frameCount;
    }
}

void sysError(synthErrno pCode) {
    printf("SYS ERROR %d\n", pCode);
    exit(pCode);
}

synthErrno sfsFread(u8 *pData, u32 pBlkIdx, u8 pBlkCnt) {
    fseek(gSfsFile, (long) pBlkIdx * BLOCK_SIZE, SEEK_SET);
    return fread(pData, 1, pBlkCnt * BLOCK_SIZE, gSfsFile) == pBlkCnt * BLOCK_SIZE ? SERR_OK : SERR_SD_READ_ERROR;
}

synthErrno sfsReadBlocks(u8 *pData, u32 pBlkIdx, u8 pBlkCnt, u16 pByteOffset, u16 pByteCount) {
    synthErrno ret;
    if (pByteOffset == 0 && pByteCount == BLOCK_SIZE)
    {
        ret = sfsFread(pData, pBlkIdx, pBlkCnt);
    }
    else
    {
        ret = sfsFread(gSfsTmpBlock, pBlkIdx, 1);

        if (pByteOffset + pByteCount > BLOCK_SIZE)
        {
            u32 remaining = pByteCount;
            u32 count     = BLOCK_SIZE - pByteOffset;
            memcpy(pData, gSfsTmpBlock + pByteOffset, count);

            pData += count;
            remaining -= count;

            u32 fullBlocksLeft = remaining / BLOCK_SIZE;
            if (fullBlocksLeft > 0)
            {
                ret = sfsFread(pData, ++pBlkIdx, fullBlocksLeft);

                pBlkIdx += fullBlocksLeft;
                pData += fullBlocksLeft * BLOCK_SIZE;
                remaining -= fullBlocksLeft * BLOCK_SIZE;
            }

            if (remaining > 0)
            {
                ret = sfsFread(gSfsTmpBlock, pBlkIdx, 1);

                memcpy(pData, gSfsTmpBlock, remaining);
            }
        }
        else
        {
            memcpy(pData, gSfsTmpBlock + pByteOffset, pByteCount);
        }
    }

    if (ret != SERR_OK)
    {
        sysError(ret);
    }
    return ret;
}

synthErrno sfsWriteBlocks(u8 *pData, u32 pBlkIdx, u8 pBlkCnt, u16 pByteOffset, u16 pByteCount) {
    printf("NO WRITING\n");
    exit(333);

    return SERR_SD_FATAL_ERROR;
}

bool run = true;

void threadInput() {
    while (run)
    {
        auto t0 = std::chrono::system_clock::now();

        if ((GetKeyState(VK_CONTROL) & GetKeyState('Q')) & 0x8000)
        {
            run = false;
        }

        for (char k = 'A'; k <= 'G'; k++)
        {
            auto i        = gNoteNameToOffsetTable[k - 'A'] + 12;
            auto key      = gSplrMainTrack->keys + i;
            key->velocity = 255;

            bool winKeyDown = GetKeyState(k) & 0x8000;
            if (key->state == SYS_KEYSTATE_DOWN && winKeyDown)
            {
                key->state = SYS_KEYSTATE_HELD;
            }

            if ((key->state == SYS_KEYSTATE_NULL || key->state == SYS_KEYSTATE_UP) && winKeyDown)
            {
                key->state = SYS_KEYSTATE_DOWN;
            }

            if ((key->state == SYS_KEYSTATE_DOWN || key->state == SYS_KEYSTATE_HELD) && !winKeyDown)
            {
                key->state = SYS_KEYSTATE_UP;
            }
        }

        while (true)
        {
            auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - t0).count();
            if (deltaMs >= 10)
            {
                break;
            }
        }
    }
}

SoundIoError main2() {
    gRawOut = std::ofstream("out-synth-player.bin", std::ios_base::out | std::ios_base::binary);
    atexit(exitCb);

    sfsInit();
    splrInit();

    // gSplrMainTrack->keys[36 - SFS_FIRST_KEY].state = SYS_KEYSTATE_HELD;
    // gSplrMainTrack->keys[36 - SFS_FIRST_KEY].velocity = 255;

    auto thInput = std::thread(threadInput);

    auto sio = soundio_create();
    if (!sio)
    {
        return SoundIoErrorInvalid;
    }

    auto ret = (SoundIoError) soundio_connect(sio);
    if (ret != SoundIoErrorNone)
    {
        return ret;
    }

    soundio_flush_events(sio);

    auto devIdx = soundio_default_output_device_index(sio);
    if (devIdx < 0)
    {
        return SoundIoErrorIncompatibleDevice;
    }

    auto dev = soundio_get_output_device(sio, devIdx);
    if (!dev)
    {
        return SoundIoErrorInvalid;
    }

    auto soundStream            = soundio_outstream_create(dev);
    soundStream->format         = SoundIoFormatS16LE;
    soundStream->write_callback = writeCbDummy;

    gStream = soundStream;

    ret = (SoundIoError) soundio_outstream_open(soundStream);
    if (ret != SoundIoErrorNone)
    {
        return ret;
    }

    if (soundStream->layout_error)
    {
        return (SoundIoError) soundStream->layout_error;
    }

    ret = (SoundIoError) soundio_outstream_start(soundStream);
    if (ret != SoundIoErrorNone)
    {
        return ret;
    }

    std::chrono::high_resolution_clock clk;

    while (run)
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        while (true)
        {
            auto t1 = std::chrono::high_resolution_clock::now();
            auto us = (f32) std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            if (us > (DMA_SIZE_SAMPLES_HALF * 1.0e6) / 48000)
            {
                break;
            }
        }

        writeCb();
    }

    thInput.join();

    sfsDeinit();

    gRawOut.close();

    soundio_outstream_destroy(soundStream);
    soundio_device_unref(dev);
    soundio_destroy(sio);

    return SoundIoErrorNone;
}

int main() {
    auto ret = main2();
    if (ret != SoundIoErrorNone)
    {
        return ret;
    }

    return ret;
}
