#include "DigitalCaliper.h"
#include <soc/gpio_struct.h>   // GPIO.in — atomic register read

// ── Static member definitions ─────────────────────────────────────────────────
DigitalCaliper* DigitalCaliper::s_instance   = nullptr;

volatile int64_t  DigitalCaliper::s_rise_us    = 0;
volatile int64_t  DigitalCaliper::s_fall_us    = 0;

volatile uint32_t DigitalCaliper::s_accumulator = 0;
volatile uint8_t  DigitalCaliper::s_bit_idx     = 0;

volatile int32_t  DigitalCaliper::s_raw_value    = 0;
volatile uint8_t  DigitalCaliper::s_raw_unit     = 0;
volatile bool     DigitalCaliper::s_ready        = false;
volatile uint32_t DigitalCaliper::s_packet_count = 0;

// ── Sync threshold ────────────────────────────────────────────────────────────
// The NPN level-shifter INVERTS both CLK and DATA signals.
// Caliper CLK HIGH (inter-packet idle) → ESP CLK LOW.
// Caliper CLK LOW  (active clock pulse) → ESP CLK HIGH.
//
// So on the ESP GPIO the inter-packet gap is a long LOW (~50-90 ms), and each
// clock pulse within a packet appears as a brief HIGH (~10 µs at 40 kHz).
// We therefore measure LOW duration (FALLING→RISING) to find packet boundaries.
//
//   acc bits  0-15 → magnitude (unsigned, 0.01 mm units)
//   acc bit   20   → sign   (0=positive, 1=negative)
//   acc bit   23   → unit   (0=mm, 1=inch)
static constexpr int64_t SYNC_LOW_THRESHOLD_US = 20000; // 20 ms

// ─────────────────────────────────────────────────────────────────────────────

DigitalCaliper::DigitalCaliper(uint8_t clk_pin, uint8_t data_pin)
    : m_clk_pin(clk_pin), m_data_pin(data_pin)
{
    s_instance = this;
}

void DigitalCaliper::begin() {
    pinMode(m_clk_pin,  INPUT);
    pinMode(m_data_pin, INPUT);
    attachInterrupt(digitalPinToInterrupt(m_clk_pin), isr_change, CHANGE);
}

// ── CHANGE ISR ────────────────────────────────────────────────────────────────
// The NPN level-shifter inverts CLK:
//   Caliper CLK HIGH (idle/inter-packet) → ESP CLK LOW
//   Caliper CLK FALLING                  → ESP CLK RISING
//   Caliper CLK RISING (data valid)      → ESP CLK FALLING
//
// RISING edge of ESP CLK = caliper CLK FALLING.
//   DATA is in transition here — do not sample.
//   Measure the preceding LOW duration (s_fall_us → now).
//   A long LOW = inter-packet gap → reset accumulator for fresh packet.
//
// FALLING edge of ESP CLK = caliper CLK RISING = DATA IS STABLE.
//   Sample DATA and accumulate 24 bits; publish on the 24th.
//     acc bits  0-15 → protocol bits  0-15 (magnitude, 0.01 mm)
//     acc bit   20   → protocol bit   20   (sign:  0=positive, 1=negative)
//     acc bit   23   → protocol bit   23   (unit:  0=mm,       1=inch)
void IRAM_ATTR DigitalCaliper::isr_change() {
    const int64_t  now     = esp_timer_get_time();
    const uint32_t gpio_in = GPIO.in;
    const uint8_t  clk     = (gpio_in >> s_instance->m_clk_pin) & 1;

    if (clk) {
        // ── Rising edge = caliper CLK FALLING ────────────────────────────────
        // DATA is in transition — do not sample.
        const int64_t low_us = now - s_fall_us;
        s_rise_us = now;
        if (low_us > SYNC_LOW_THRESHOLD_US) {
            // Inter-packet gap just ended — reset for fresh packet.
            s_accumulator = 0;
            s_bit_idx     = 0;
        }

    } else {
        // ── Falling edge = caliper CLK RISING = DATA STABLE ──────────────────
        s_fall_us = now;

        if (s_bit_idx >= 24) return;  // guard against spurious extra edges

        const uint8_t data = (gpio_in >> s_instance->m_data_pin) & 1;
        if (data == 0) {
            // Active-low (inverted): ESP DATA LOW → caliper DATA HIGH → bit = 1.
            s_accumulator |= (1UL << s_bit_idx);
        }
        if (++s_bit_idx == 24) {
            publish(s_accumulator);
        }
    }
}

// ── Decode and publish ────────────────────────────────────────────────────────
void DigitalCaliper::publish(uint32_t acc) {
    const uint32_t magnitude = acc & 0xFFFF;         // protocol bits 0-15
    const bool     negative  = (acc >> 20) & 1;      // protocol bit 20 (sign)
    const uint8_t  unit      = (acc >> 23) & 1;      // protocol bit 23 (unit)

    const int32_t signed_val = negative
        ? -(int32_t)magnitude
        :  (int32_t)magnitude;

    s_raw_value    = signed_val;
    s_raw_unit     = unit;
    s_packet_count = s_packet_count + 1;  // diagnostic
    s_ready        = true;                // publish last
}

// ── read() — call from loop() ─────────────────────────────────────────────────
bool DigitalCaliper::read(CaliperReading& out) {
    if (!s_ready) return false;
    s_ready = false;   // consume before copying so no double-read

    const int32_t  v    = s_raw_value;
    const uint8_t  u    = s_raw_unit;
    const bool     neg  = (v < 0);
    const int32_t  absv = neg ? -v : v;
    const CaliperUnit unit = (u == 1) ? CaliperUnit::INCH : CaliperUnit::MM;

    int16_t  whole;
    uint16_t frac;

    if (unit == CaliperUnit::INCH) {
        // 1 LSB = 0.0005 inch → multiply by 5 to get ten-thousandths of an inch.
        const int32_t ten_thou = (int32_t)absv * 5;
        whole = (int16_t)(ten_thou / 10000);
        frac  = (uint16_t)(ten_thou % 10000);
    } else {
        // 1 LSB = 0.01 mm → hundredths of a mm.
        whole = (int16_t)(absv / 100);
        frac  = (uint16_t)(absv % 100);
    }

    if (neg) whole = -whole;  // restore sign (frac stays unsigned)

    // const members delete operator= — construct in-place via placement new.
    new (&out) CaliperReading{ whole, frac, neg, unit };
    return true;
}
