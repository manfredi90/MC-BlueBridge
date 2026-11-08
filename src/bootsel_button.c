#include "bootsel_button.h"
#include "bluebridge_platform.h"
#include "bluebridge_state.h"
#include "led_status.h"
#include "usb_device.h"
#include "btstack_run_loop.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "pico/flash.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

// BOOTSEL button:
//   1 short press           start/stop pairing
//   2 short presses         X-Input output mode
//   3 short presses         DualSense output mode
//   hold 1 s                start/stop pairing
//   hold 5 s                back to MiSTer mode (from X-Input, Xbox 360 or DualSense mode)

#define BOOTSEL_POLL_MS 20u
#define BOOTSEL_DEBOUNCE_MS 30u
#define BOOTSEL_CLICK_GAP_MS 400u
#define BOOTSEL_HOLD_PAIRING_MS 1000u
#define BOOTSEL_HOLD_RECOVERY_MS 5000u

static btstack_timer_source_t timer;
static bool initialized;
static bool observed;
static bool stable;
static uint32_t changed_at;
static uint32_t pressed_at;
static bool pairing_action;
static bool recovery_action;
static uint8_t clicks;
static uint32_t released_at;

static void __no_inline_not_in_flash_func(sample_bootsel)(void* context) {
    uint32_t saved = ioqspi_hw->io[1].ctrl;
    ioqspi_hw->io[1].ctrl = (saved & ~IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS) |
        (GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB);
    for (volatile unsigned delay = 0; delay < 1000; ++delay) {}
    bool pressed = (sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS) == 0;
    ioqspi_hw->io[1].ctrl = saved;
    *(bool*)context = pressed;
}

static void toggle_pairing(void) {
    bb_runtime_state_t state;
    bb_state_get(&state);
    if (state.pairing) bb_platform_stop_pairing();
    else bb_platform_start_pairing();
}

static void select_output_mode(bb_output_mode_t mode) {
    if (bb_output_mode() == mode) return;
    if (!bb_output_mode_set(mode)) return;
    bb_led_signal_profile_changed();
    bb_usb_request_reenumeration();
}

static void handle_clicks(uint8_t count) {
    if (count == 1) toggle_pairing();
    else if (count == 2) select_output_mode(BB_OUTPUT_XINPUT);
    else if (count == 3) select_output_mode(BB_OUTPUT_DUALSENSE);
}

void bb_bootsel_poll(uint32_t now) {
    bool pressed = false;
    if (flash_safe_execute(sample_bootsel, &pressed, 1) != PICO_OK) return;
    if (!initialized) {
        initialized = true;
        observed = pressed;
        stable = pressed;
        changed_at = now;
        pressed_at = pressed ? now : 0;
        // A button already held at boot is not a click.
        pairing_action = pressed;
        recovery_action = pressed;
        return;
    }
    if (pressed != observed) {
        observed = pressed;
        changed_at = now;
        return;
    }
    if (pressed != stable && (uint32_t)(now - changed_at) >= BOOTSEL_DEBOUNCE_MS) {
        stable = pressed;
        if (stable) {
            pressed_at = now;
            pairing_action = false;
            recovery_action = false;
        } else if (!pairing_action && !recovery_action) {
            // Short press released: count it as a click.
            if (clicks < 255) ++clicks;
            released_at = now;
        }
        return;
    }
    if (!stable) {
        if (clicks && (uint32_t)(now - released_at) >= BOOTSEL_CLICK_GAP_MS) {
            uint8_t count = clicks;
            clicks = 0;
            handle_clicks(count);
        }
        return;
    }
    uint32_t held = (uint32_t)(now - pressed_at);
    if (!pairing_action && held >= BOOTSEL_HOLD_PAIRING_MS) {
        // A long press cancels any clicks counted just before it.
        pairing_action = true;
        clicks = 0;
        toggle_pairing();
    }
    if (!recovery_action && held >= BOOTSEL_HOLD_RECOVERY_MS &&
        (bb_output_mode() == BB_OUTPUT_XBOX360 || bb_output_mode() == BB_OUTPUT_DUALSENSE ||
         bb_output_mode() == BB_OUTPUT_XINPUT)) {
        recovery_action = true;
        bb_platform_stop_pairing();
        if (bb_output_mode_set(BB_OUTPUT_MISTER)) bb_usb_request_reenumeration();
    }
}

static void timer_handler(btstack_timer_source_t* ts) {
    bb_bootsel_poll(to_ms_since_boot(get_absolute_time()));
    btstack_run_loop_set_timer(ts, BOOTSEL_POLL_MS);
    btstack_run_loop_add_timer(ts);
}

void bb_bootsel_init(void) {
    btstack_run_loop_set_timer_handler(&timer, timer_handler);
    btstack_run_loop_set_timer(&timer, BOOTSEL_POLL_MS);
    btstack_run_loop_add_timer(&timer);
}
