#include "jd_protocol.h"
#include "interfaces/jd_sensor.h"
#include "interfaces/jd_pins.h"
#include "interfaces/jd_adc.h"
#include "interfaces/jd_console.h"

#define EVT_DOWN 1
#define EVT_UP 2
#define EVT_CLICK 3
#define EVT_LONG_CLICK 4

#define PRESS_THRESHOLD 70
#define PRESS_TICKS 3

#define SAMPLING_US 1000
#define SAMPLE_WINDOW 19

#define BASELINE_SAMPLES 20
#define BASELINE_SUPER_SAMPLES 10
#define BASELINE_FREQ (1000000 / BASELINE_SAMPLES)

#define SWIPE_DURATION_MIN 10
#define SWIPE_DURATION_MAX 500
#define SWIPE_DELTA_MIN 10
#define SWIPE_DELTA_MAX 500

typedef struct pin_desc {
    uint8_t pin;
    int8_t ticks_pressed;
    uint32_t start_press;
    uint32_t end_press;
    uint16_t reading;
    uint16_t readings[SAMPLE_WINDOW];
    uint16_t baseline_samples[BASELINE_SAMPLES];
    uint16_t baseline_super_samples[BASELINE_SUPER_SAMPLES];
    uint16_t baseline;
} pin_t;

struct srv_state {
    SENSOR_COMMON;
    uint8_t numpins;
    uint8_t num_baseline_samples;
    pin_t *pins;
    int32_t *readings;
    uint32_t nextSample;
    uint32_t next_baseline_sample;
};

static void sort(uint16_t *a, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--)
            if (a[j] < a[j - 1]) {
                uint16_t tmp = a[j];
                a[j] = a[j - 1];
                a[j - 1] = tmp;
            }
}

static uint16_t add_sample(uint16_t *samples, uint8_t len, uint16_t sample) {
    uint16_t rcopy[len];
    memcpy(rcopy, samples + 1, (len - 1) * 2);
    rcopy[len - 1] = sample;
    memcpy(samples, rcopy, len * 2);
    sort(rcopy, len);
    return rcopy[len >> 1];
}

static uint16_t read_pin(uint8_t pin) {
    for (;;) {
        pin_set(pin, 1);
        uint64_t t0 = tim_get_micros();
        pin_setup_output(pin);
        pin_setup_analog_input(pin);
        target_wait_us(50);
        int r = adc_read_pin(pin);
        int dur = tim_get_micros() - t0;
        // only return results from runs not interrupted
        if (dur < 1800)
            return r;
    }
}

static uint16_t read_pin_avg(uint8_t pin) {
    int n = 3;
    uint16_t arr[n];
    for (int i = 0; i < n; ++i) {
        arr[i] = read_pin(pin);
    }
    sort(arr, n);
    return arr[n >> 1];
}

static void detect_swipe(srv_t *state) {
    int delta = 0;
    for (int i = 0; i < state->numpins; ++i) {
        pin_t *p = &state->pins[i];
        uint32_t duration = p->end_press - p->start_press;
        if (!(SWIPE_DURATION_MIN <= duration && duration <= SWIPE_DURATION_MAX)) {
            jdcon_log("d[%d]: %d", i, duration);
            return;
        }
        if (i > 0) {
            pin_t *p0 = &state->pins[i - 1];
            int d0 = p->start_press - p0->start_press;
            int d1 = p->end_press - p0->end_press;
            jdcon_log("d0 %d %d", d0, d1);
            if (d0 < 0) {
                if (d1 > 0 || delta > 0)
                    return;
                delta = -1;
                d0 = -d0;
                d1 = -d1;
            } else {
                if (d1 < 0 || delta < 0)
                    return;
                delta = 1;
            }
            if (!(SWIPE_DELTA_MIN <= d0 && d0 <= SWIPE_DELTA_MAX) ||
                !(SWIPE_DELTA_MIN <= d1 && d1 <= SWIPE_DELTA_MAX))
                return;
        }
    }
    jdcon_warn("swp %d", delta);
}

static void update(srv_t *state) {
    uint32_t now_ms = tim_get_micros() >> 10; // roughly milliseconds

    for (int i = 0; i < state->numpins; ++i) {
        pin_t *p = &state->pins[i];
        p->reading = add_sample(p->readings, SAMPLE_WINDOW, read_pin_avg(p->pin));
        state->readings[i] = p->reading - p->baseline;

        bool was_pressed = p->ticks_pressed >= PRESS_TICKS;

        if (p->reading - p->baseline > PRESS_THRESHOLD) {
            p->ticks_pressed++;
        } else {
            p->ticks_pressed--;
        }
        if (p->ticks_pressed > PRESS_TICKS * 2)
            p->ticks_pressed = PRESS_TICKS * 2;
        else if (p->ticks_pressed < 0)
            p->ticks_pressed = 0;

        bool is_pressed = p->ticks_pressed >= PRESS_TICKS;
        if (is_pressed != was_pressed) {
            if (is_pressed) {
                p->start_press = now_ms;
            } else {
                p->end_press = now_ms;
                jdcon_log("press p%d %dms", i, now_ms - p->start_press);
                detect_swipe(state);
            }
        }
    }
}

static void update_baseline(srv_t *state) {
    state->num_baseline_samples++;
    if (state->num_baseline_samples >= BASELINE_SAMPLES)
        state->num_baseline_samples = 0;
    for (int i = 0; i < state->numpins; ++i) {
        pin_t *p = &state->pins[i];
        uint16_t tmp = add_sample(p->baseline_samples, BASELINE_SAMPLES, p->reading);
        if (state->num_baseline_samples == 0) {
            p->baseline = add_sample(p->baseline_super_samples, BASELINE_SUPER_SAMPLES, tmp);
            if (i == 1)
                jdcon_log("re-calib: %d %d", state->pins[0].baseline, state->pins[1].baseline);
        }
    }
}

static void calibrate(srv_t *state) {
    for (int k = 0; k < BASELINE_SUPER_SAMPLES; ++k) {
        for (int j = 0; j < BASELINE_SAMPLES; ++j) {
            for (int i = 0; i < state->numpins; ++i) {
                pin_t *p = &state->pins[i];
                p->reading = read_pin_avg(p->pin);
            }
            update_baseline(state);
        }
        target_wait_us(100);
    }
    DMESG("calib: %d", state->pins[0].baseline);
}

void multitouch_process(srv_t *state) {
    if (jd_should_sample(&state->nextSample, SAMPLING_US * 9 / 10)) {
        update(state);
        if (jd_should_sample(&state->next_baseline_sample, BASELINE_FREQ))
            update_baseline(state);
        sensor_process_simple(state, state->readings, state->numpins * sizeof(int32_t));
    }
}

void multitouch_handle_packet(srv_t *state, jd_packet_t *pkt) {
    sensor_handle_packet_simple(state, pkt, state->readings, state->numpins * sizeof(int32_t));
}

SRV_DEF(multitouch, JD_SERVICE_CLASS_MULTITOUCH);

void multitouch_init(const uint8_t *pins) {
    SRV_ALLOC(multitouch);

    tim_max_sleep = SAMPLING_US;

    while (pins[state->numpins] != 0xff)
        state->numpins++;
    state->pins = jd_alloc(state->numpins * sizeof(pin_t));
    state->readings = jd_alloc(state->numpins * sizeof(int32_t));

    for (int i = 0; i < state->numpins; ++i) {
        state->pins[i].pin = pins[i];
        pin_setup_input(pins[i], 0);
    }

    state->streaming_interval = 50;

    calibrate(state);
}
