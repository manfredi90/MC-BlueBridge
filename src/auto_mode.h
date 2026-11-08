#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bluebridge_state.h"

// Automatic output mode (test build): probes the host by trying X-Input, then
// DualSense, then settling on Nintendo Switch, and keeps the first mode whose
// host driver shows up.

typedef enum {
    BB_AUTO_EVIDENCE_XINPUT_POLL = 1,     // host polled the X-Input IN endpoint
    BB_AUTO_EVIDENCE_XINPUT_OUT = 2,      // host sent an X-Input command (LED/rumble)
    BB_AUTO_EVIDENCE_WINDOWS = 3,         // Windows asked for the MS OS string (first connection)
    BB_AUTO_EVIDENCE_DUALSENSE_FEATURE = 4 // host read a DualSense feature report
} bb_auto_evidence_t;

void bb_auto_task(void);
void bb_auto_evidence(bb_auto_evidence_t evidence);
// True while X-Input is being probed: keep an input report queued so that a host
// driver that polls the controller completes it.
bool bb_auto_probing_xinput(void);
const char* bb_auto_stage_name(void);
const char* bb_auto_detail(void);
