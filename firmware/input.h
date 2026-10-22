#pragma once
/* Keybed, button matrix, panel buttons and 4D-encoder rotation.
   Scanned from the 10 kHz SysTick interrupt; events are queued for the main loop.
   Pin and matrix details: re/hardware_map.md sections 1-3. */
#include <stdint.h>

enum input_ev_type { IN_KEY, IN_BUTTON, IN_ENCODER };

typedef struct {
    uint8_t type;      /* input_ev_type */
    uint8_t index;     /* key 0..31 / button index (hardware_map 2) */
    uint8_t value;     /* key: velocity (0 = note off); button: 1 down, 0 up */
    int8_t delta;      /* encoder: edges since last event, signed */
} input_ev_t;

void input_init(void);
int input_next(input_ev_t *ev);     /* 1 if an event was returned */
extern volatile uint32_t input_ms;  /* milliseconds since input_init */
void input_systick_isr(void);
