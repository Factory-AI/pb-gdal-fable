#pragma once

// Replica of GDALTermProgress writing to stdout.
struct TermProgress
{
    int lastTick = -1;
    double startTime = 0;

    void update(double complete);
};

extern int g_termTickFloor;
