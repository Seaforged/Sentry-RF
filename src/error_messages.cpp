#include "error_messages.h"
#include <RadioLib.h>
#include <stdio.h>

void formatRadioError(char* buf, size_t bufLen, int errCode) {
    const char* label;
    switch (errCode) {
        case RADIOLIB_ERR_CHIP_NOT_FOUND:
            label = "Radio chip not found";
            break;
        case RADIOLIB_ERR_INVALID_FREQUENCY:
            label = "Invalid frequency";
            break;
        case RADIOLIB_ERR_INVALID_FREQUENCY_DEVIATION:
            label = "Invalid freq dev";
            break;
        case RADIOLIB_ERR_SPI_CMD_FAILED:
            label = "SPI command failed";
            break;
        // TODO(v1.6.1): extend as new codes appear in field logs.
        // Full list in .pio/libdeps/*/RadioLib/src/TypeDef.h
        default:
            label = nullptr;
            break;
    }

    if (label != nullptr) {
        snprintf(buf, bufLen, "%s", label);
    } else {
        snprintf(buf, bufLen, "Err: %d", errCode);
    }
}
