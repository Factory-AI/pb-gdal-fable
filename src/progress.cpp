#include "progress.h"
#include <cstdio>
#include <sys/time.h>

static double nowSeconds()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

// mid-pipeline writes leave a partially printed progress line behind;
// later bars continue it instead of restarting at zero
int g_termTickFloor = 0;

void TermProgress::update(double complete)
{
    int thisTick = static_cast<int>(complete * 40.0);
    if (thisTick < 0)
        thisTick = 0;
    if (thisTick > 40)
        thisTick = 40;
    // a large backwards jump after a completed line starts a fresh line
    if (thisTick < lastTick && lastTick >= 39)
        lastTick = -1;
    if (lastTick < 0 && g_termTickFloor > 0)
    {
        lastTick = g_termTickFloor;
        startTime = nowSeconds();
    }
    if (thisTick <= lastTick)
        return;
    if (lastTick < 0)
        startTime = nowSeconds();
    while (thisTick > lastTick)
    {
        ++lastTick;
        if (lastTick % 4 == 0)
            printf("%d", lastTick / 4 * 10);
        else
            printf(".");
    }
    if (thisTick == 40)
    {
        // long-running bars report their duration (5-second threshold)
        double elapsed = nowSeconds() - startTime;
        if (elapsed >= 5.0)
        {
            unsigned s = (unsigned)elapsed;
            printf(" - done in %02u:%02u:%02u.\n", s / 3600,
                   (s % 3600) / 60, s % 60);
        }
        else
            printf(" - done.\n");
    }
    fflush(stdout);
}
