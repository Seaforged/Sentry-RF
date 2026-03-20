#include "drone_signatures.h"
#include <math.h>

const DroneProtocol DRONE_PROTOCOLS[] = {
    { "ELRS_915",  902.0,   928.0,   0.325, 80  },
    { "ELRS_868",  860.0,   886.0,   0.520, 50  },
    { "CRSF_915",  902.165, 927.905, 0.260, 100 },
    { "CRSF_868",  860.165, 885.905, 0.260, 100 },
};

const int DRONE_PROTOCOL_COUNT = sizeof(DRONE_PROTOCOLS) / sizeof(DRONE_PROTOCOLS[0]);

FreqMatch matchFrequency(float freqMHz) {
    FreqMatch best = { nullptr, 0, 9999.0 };

    for (int p = 0; p < DRONE_PROTOCOL_COUNT; p++) {
        const DroneProtocol& proto = DRONE_PROTOCOLS[p];

        if (freqMHz < proto.bandStart || freqMHz > proto.bandEnd) continue;

        // Nearest channel index from band start
        float offset = freqMHz - proto.bandStart;
        int ch = (int)(offset / proto.channelSpacing + 0.5f);

        if (ch < 0 || ch >= proto.numChannels) continue;

        float chCenter = proto.bandStart + (ch * proto.channelSpacing);
        float devKHz = fabsf(freqMHz - chCenter) * 1000.0f;

        // Only valid if within half the channel spacing
        float maxDevKHz = (proto.channelSpacing * 1000.0f) / 2.0f;
        if (devKHz < maxDevKHz && devKHz < best.deviationKHz) {
            best.protocol = &proto;
            best.channel = (uint16_t)ch;
            best.deviationKHz = devKHz;
        }
    }

    return best;
}
