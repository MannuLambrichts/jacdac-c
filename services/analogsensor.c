// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "jd_services.h"
#include "interfaces/jd_pins.h"
#include "interfaces/jd_adc.h"

struct srv_state {
    ANALOG_SENSOR_STATE;
};

static void analog_update(srv_t *state) {
    const analog_config_t *cfg = state->config;
    pin_setup_output(cfg->pinH);
    pin_set(cfg->pinH, 1);
    pin_setup_output(cfg->pinL);
    pin_set(cfg->pinL, 0);

    int scale = cfg->scale;
    if (!scale)
        scale = 1024;
    int v = cfg->offset + (((int)adc_read_pin(cfg->pinM) * scale) >> 10);
    if (v < 0)
        v = 0;
    else if (v > 0xffff)
        v = 0xffff;
    state->sample = v;

    // save power
    pin_setup_analog_input(cfg->pinH);
    pin_setup_analog_input(cfg->pinL);
}

void analog_process(srv_t *state) {
    if (state->got_query && !state->inited) {
        state->inited = true;
        analog_update(state);
    }

    if (jd_should_sample(&state->nextSample, 9000) && state->inited)
        analog_update(state);

    sensor_process_simple(state, &state->sample, sizeof(state->sample));
}

void analog_handle_packet(srv_t *state, jd_packet_t *pkt) {
    if (state->config->variant && pkt->service_command == JD_GET(JD_REG_VARIANT)) {
        jd_respond_u8(pkt, state->config->variant);
        return;
    }
    sensor_handle_packet_simple(state, pkt, &state->sample, sizeof(state->sample));
}

void analog_init(const srv_vt_t *vt, const analog_config_t *cfg) {
    srv_t *state = jd_allocate_service(vt);
    state->config = cfg;
}