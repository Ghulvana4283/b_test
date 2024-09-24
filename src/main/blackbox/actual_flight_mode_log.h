#pragma once
extern uint32_t blackboxActualFlightModeFlags;
typedef enum {
    ACTUAL_ANGLE_MODE       = (1 << 0),
    ACTUAL_HORIZON_MODE     = (1 << 1),
    ACTUAL_GPS_RESCUE_MODE  = (1 << 2),
    ACTUAL_FAILSAFE_MODE    = (1 << 3),
    ACTUAL_ALT_HOLD_MODE    = (1 << 4),
    ACTUAL_MAG_MODE         = (1 << 5),
    ACTUAL_HEADFREE_MODE    = (1 << 6),
    ACTUAL_PASSTHRU_MODE    = (1 << 7),
    ACTUAL_AIR_MODE         = (1 << 8),
    ACTUAL_AUTOLAUNCH_MODE  = (1 << 9),
    ACTUAL_ACROTRAINER_MODE = (1 << 10)
} actualFlightModeFlags_e;

#define SET_ACTUAL_FLIGHT_MODE_STATE(mode)  blackboxActualFlightModeFlags |= mode
