#ifndef DRONE_SIGNATURES_H
#define DRONE_SIGNATURES_H

#include <stdint.h>

struct DroneProtocol {
    const char* name;
    float bandStart;      // MHz
    float bandEnd;        // MHz
    float channelSpacing; // MHz
    uint16_t numChannels;
};

struct FreqMatch {
    const DroneProtocol* protocol;  // nullptr if no match
    uint16_t channel;               // channel number within protocol
    float deviationKHz;             // offset from nearest channel center
};

// Number of known protocols in the database
extern const int DRONE_PROTOCOL_COUNT;
extern const DroneProtocol DRONE_PROTOCOLS[];

// Find the best protocol/channel match for a given frequency.
// Returns no-match (protocol=nullptr) if deviation exceeds half the channel spacing.
FreqMatch matchFrequency(float freqMHz);

#endif // DRONE_SIGNATURES_H
