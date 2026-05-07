#include <Arduino.h>
#include "DigitalCaliper.h"

static constexpr uint8_t PIN_CLK  = 15;
static constexpr uint8_t PIN_DATA =  7;

static DigitalCaliper caliper(PIN_CLK, PIN_DATA);

void setup() {
    Serial.begin(115200);
    caliper.begin();
}

void loop() {
    CaliperReading r;
    if (caliper.read(r)) {
        const bool neg = r.negative || r.whole < 0;
        if (r.unit == CaliperUnit::MM) {
            Serial.printf("[caliper] %c%d.%02u mm\n",
                neg ? '-' : '+', abs((int)r.whole), (unsigned)r.frac);
        } else {
            Serial.printf("[caliper] %c%d.%04u\"\n",
                neg ? '-' : '+', abs((int)r.whole), (unsigned)r.frac);
        }
    }
}
