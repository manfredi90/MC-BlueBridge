// Automatic output mode (test build).
//
// The adapter cannot ask the host what it is, but it can watch how the host
// treats each USB identity:
//   1. X-Input: Windows and Linux (MiSTer) load their Xbox driver, which polls
//      the controller and sends it the player LED command. On its first
//      connection Windows also asks for the Microsoft OS string descriptor
//      before installing the driver, which can take a while.
//   2. DualSense: macOS has no X-Input driver, so step 1 times out; its
//      DualSense driver then reads the controller's feature reports.
//   3. Nintendo Switch: supports neither of the above over USB, so both steps
//      time out and the adapter settles on Switch mode.
// The first mode with evidence of a host driver is kept until the adapter loses
// the host for a few seconds (e.g. moved to another device), which restarts the
// detection.

#include "auto_mode.h"
#include "usb_device.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define AUTO_MOUNT_SETTLE_MS 200u   // ignore the old connection right after re-enumerating
#define AUTO_WAIT_AFTER_MOUNT_MS 2500u
#define AUTO_WAIT_NO_MOUNT_MS 4000u
#define AUTO_LOST_HOST_MS 5000u

typedef enum {
    STAGE_OFF,
    STAGE_XINPUT,
    STAGE_DUALSENSE,
    STAGE_LOCKED
} auto_stage_t;

static auto_stage_t stage = STAGE_OFF;
static uint32_t generation_seen;
static uint32_t stage_started;
static uint32_t mounted_at;
static uint32_t unmounted_since;
static bool was_mounted;
static volatile uint8_t pending_evidence;
static const char* detail = "";

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void start_stage(auto_stage_t next, bb_output_mode_t mode, bool reenumerate, uint32_t now) {
    stage = next;
    bb_output_auto_set_effective(mode);
    stage_started = now;
    mounted_at = 0;
    was_mounted = false;
    unmounted_since = 0;
    pending_evidence = 0;
    if (reenumerate) bb_usb_request_reenumeration();
}

static void lock(bb_output_mode_t mode, const char* why, bool reenumerate, uint32_t now) {
    if (bb_output_mode() != mode) {
        bb_output_auto_set_effective(mode);
        reenumerate = true;
    }
    stage = STAGE_LOCKED;
    detail = why;
    stage_started = now;
    was_mounted = false;
    unmounted_since = 0;
    if (reenumerate) bb_usb_request_reenumeration();
}

static const char* evidence_name(uint8_t evidence) {
    switch (evidence) {
        case BB_AUTO_EVIDENCE_XINPUT_POLL: return "x-input driver polled";
        case BB_AUTO_EVIDENCE_XINPUT_OUT: return "x-input driver command";
        case BB_AUTO_EVIDENCE_WINDOWS: return "windows os descriptor";
        case BB_AUTO_EVIDENCE_DUALSENSE_FEATURE: return "dualsense feature read";
        default: return "";
    }
}

void bb_auto_evidence(bb_auto_evidence_t evidence) {
    if (stage == STAGE_XINPUT && evidence != BB_AUTO_EVIDENCE_DUALSENSE_FEATURE) pending_evidence = (uint8_t)evidence;
    else if (stage == STAGE_DUALSENSE && evidence == BB_AUTO_EVIDENCE_DUALSENSE_FEATURE) pending_evidence = (uint8_t)evidence;
}

bool bb_auto_probing_xinput(void) {
    return stage == STAGE_XINPUT;
}

void bb_auto_task(void) {
    uint32_t now = now_ms();
    if (bb_output_mode_value() != BB_OUTPUT_AUTO) {
        stage = STAGE_OFF;
        return;
    }
    uint32_t generation = bb_output_auto_generation();
    if (stage == STAGE_OFF || generation != generation_seen) {
        // At boot the adapter already enumerated as X-Input; when Auto is selected
        // at runtime, re-enumerate to start from X-Input.
        bool reenumerate = stage != STAGE_OFF || generation != 0;
        generation_seen = generation;
        detail = "";
        start_stage(STAGE_XINPUT, BB_OUTPUT_XINPUT, reenumerate, now);
        return;
    }

    bool settled = (uint32_t)(now - stage_started) >= AUTO_MOUNT_SETTLE_MS;
    bool mounted = settled && tud_mounted();

    if (stage == STAGE_LOCKED) {
        // Restart detection if the host goes away for a while (adapter moved to
        // another device while powered, host rebooted into another system...).
        if (mounted) {
            was_mounted = true;
            unmounted_since = 0;
        } else if (was_mounted && settled) {
            if (!unmounted_since) unmounted_since = now;
            else if ((uint32_t)(now - unmounted_since) >= AUTO_LOST_HOST_MS) {
                detail = "";
                start_stage(STAGE_XINPUT, BB_OUTPUT_XINPUT, true, now);
            }
        }
        return;
    }

    if (mounted && !mounted_at) mounted_at = now;
    uint8_t evidence = pending_evidence;
    if (evidence) {
        lock(stage == STAGE_XINPUT ? BB_OUTPUT_XINPUT : BB_OUTPUT_DUALSENSE, evidence_name(evidence), false, now);
        return;
    }
    bool timed_out = mounted_at ? (uint32_t)(now - mounted_at) >= AUTO_WAIT_AFTER_MOUNT_MS
                                : (uint32_t)(now - stage_started) >= AUTO_WAIT_NO_MOUNT_MS;
    if (!timed_out) return;
    if (stage == STAGE_XINPUT) start_stage(STAGE_DUALSENSE, BB_OUTPUT_DUALSENSE, true, now);
    else lock(BB_OUTPUT_SWITCH, "no x-input or dualsense driver", true, now);
}

const char* bb_auto_stage_name(void) {
    switch (stage) {
        case STAGE_XINPUT: return "probing x-input";
        case STAGE_DUALSENSE: return "probing dualsense";
        case STAGE_LOCKED: return "locked";
        default: return "off";
    }
}

const char* bb_auto_detail(void) {
    return detail;
}
