#ifndef ERROR_MESSAGES_H
#define ERROR_MESSAGES_H

#include <stddef.h>

// Writes a short, operator-friendly description of a RadioLib error code
// into `buf`. For codes not in the lookup table, writes "Err: %d" as a
// fallback so the numeric code stays visible for field debugging.
//
// Caller's buffer should be at least 22 bytes — enough for one OLED line
// at font size 1 (21 chars + null terminator).
void formatRadioError(char* buf, size_t bufLen, int errCode);

#endif // ERROR_MESSAGES_H
