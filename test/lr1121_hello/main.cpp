#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// Expose getRssiInst() which is protected in LR11x0
class LR1121_RSSI : public LR1121 {
public:
    using LR1121::LR1121;
    float getInstantRSSI() {
        float rssi = 0;
        getRssiInst(&rssi);
        return rssi;
    }
};

SPIClass loraSPI(HSPI);
Module radioMod(7, 36, 8, 34, loraSPI);  // CS=7, DIO9=36, RST=8, BUSY=34
LR1121_RSSI radio(&radioMod);

static const uint32_t rfswitch_dio_pins[] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
    RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};
static const Module::RfSwitchMode_t rfswitch_table[] = {
    { LR11x0::MODE_STBY,  { LOW,  LOW  } },
    { LR11x0::MODE_RX,    { HIGH, LOW  } },
    { LR11x0::MODE_TX,    { LOW,  HIGH } },
    { LR11x0::MODE_TX_HP, { LOW,  HIGH } },
    { LR11x0::MODE_TX_HF, { LOW,  LOW  } },
    { LR11x0::MODE_GNSS,  { LOW,  LOW  } },
    { LR11x0::MODE_WIFI,  { LOW,  LOW  } },
    END_OF_MODE_TABLE,
};

void setup() {
    Serial.begin(115200);
    delay(3000);
    Serial.println("\n========== LR1121 HELLO HARDWARE ==========");

    loraSPI.begin(5, 3, 6, 7);
    radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);

    // LR1121::beginGFSK(freq, br, freqDev, rxBw, power, preambleLength, tcxoVoltage)
    int state = radio.beginGFSK(915.0, 4.8, 50.0, 156.2, 10, 16, 3.0);
    Serial.printf("beginGFSK(915MHz, br=4.8, dev=50, rxBw=156.2, pwr=10, tcxo=3.0): %d\n", state);

    if (state == RADIOLIB_ERR_NONE) {
        radio.startReceive();
        Serial.println("\n--- GFSK Instantaneous RSSI at 915 MHz ---");
        for (int i = 0; i < 5; i++) {
            Serial.printf("  RSSI: %.1f dBm\n", radio.getInstantRSSI());
            delay(500);
        }

        // Frequency sweep
        Serial.println("\n--- GFSK Frequency Sweep ---");
        float freqs[] = {868.0, 880.0, 900.0, 915.0, 920.0, 928.0, 945.0};
        for (int i = 0; i < 7; i++) {
            radio.setFrequency(freqs[i]);
            delay(5);
            Serial.printf("  %.1f MHz: %.1f dBm\n", freqs[i], radio.getInstantRSSI());
        }
    } else {
        Serial.printf("GFSK FAILED: %d\n", state);
    }

    // LoRa mode — CAD test
    Serial.println("\n--- LoRa Mode ---");
    state = radio.begin(915.0, 500.0, 7, 5, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, 10, 8, 3.0);
    Serial.printf("begin(LoRa): %d\n", state);

    if (state == RADIOLIB_ERR_NONE) {
        for (int i = 0; i < 5; i++) {
            state = radio.scanChannel();
            const char* result = (state == RADIOLIB_LORA_DETECTED) ? "DETECTED" :
                                 (state == RADIOLIB_CHANNEL_FREE)  ? "FREE" : "ERROR";
            Serial.printf("  CAD[%d]: %s (%d)\n", i, result, state);
            delay(100);
        }

        radio.startReceive();
        Serial.println("\n--- LoRa Instantaneous RSSI ---");
        for (int i = 0; i < 5; i++) {
            Serial.printf("  RSSI: %.1f dBm\n", radio.getInstantRSSI());
            delay(500);
        }
    }

    // Back to GFSK for continuous
    radio.beginGFSK(915.0, 4.8, 50.0, 156.2, 10, 16, 3.0);
    radio.startReceive();
    Serial.println("\n--- Continuous GFSK RSSI ---");
}

void loop() {
    Serial.printf("RSSI: %.1f dBm\n", radio.getInstantRSSI());
    delay(1000);
}
