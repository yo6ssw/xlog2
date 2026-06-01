#pragma once

#include <string>

// A single DX-cluster spot, parsed from a "DX de" line.
struct DxSpot {
    double      freqKHz = 0.0;  // frequency as spotted, in kHz
    std::string dxCall;         // the spotted (DX) station
    std::string spotter;        // who reported it
    std::string comment;        // free-form remark
    std::string timeUtc;        // e.g. "1432Z"
    std::string band;           // derived from freqKHz, e.g. "20m"
};
