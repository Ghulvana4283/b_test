/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include <platform.h>

#include "build_config.h"
#include "debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/sensor.h"
#include "drivers/gyro_sync.h"

#include "drivers/accgyro.h"
#include "sensors/sensors.h"
#include "sensors/gyro.h"
#include "sensors/acceleration.h"

#include "rx/rx.h"

#include "io/rc_controls.h"
#include "io/gps.h"

#include "flight/pid.h"
#include "flight/imu.h"
#include "flight/navigation.h"
#include "flight/gtune.h"

#include "config/runtime_config.h"

extern uint8_t motorCount;
uint32_t targetPidLooptime;

int16_t axisPID[3];

#ifdef BLACKBOX
int32_t axisPID_P[3], axisPID_I[3], axisPID_D[3];
#endif

// PIDweight is a scale factor for PIDs which is derived from the throttle and TPA setting, and 100 = 100% scale means no PID reduction
uint8_t PIDweight[3];

static int32_t errorGyroI[3];
#ifndef SKIP_PID_LUXFLOAT
static float errorGyroIf[3];
#endif

static void pidMultiWiiRewrite(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig,
        uint16_t max_angle_inclination, const rollAndPitchTrims_t *angleTrim, const rxConfig_t *rxConfig);

pidControllerFuncPtr pid_controller = pidMultiWiiRewrite; // which pid controller are we using, defaultMultiWii

void setTargetPidLooptime(uint8_t pidProcessDenom) {
	targetPidLooptime = targetLooptime * pidProcessDenom;
}

float calculateExpoPlus(int axis, const rxConfig_t *rxConfig) {
    float propFactor;
    float superExpoFactor;

    if (axis == YAW && !rxConfig->superExpoYawMode) {
        propFactor = 1.0f;
    } else {
        superExpoFactor = (axis == YAW) ? rxConfig->superExpoFactorYaw : rxConfig->superExpoFactor;
        propFactor = constrainf(1.0f - ((superExpoFactor / 100.0f) * (ABS(rcCommand[axis]) / 500.0f)), 0.0f, 1.0f);
    }

    return propFactor;
}

uint16_t getDynamicKp(int axis, const pidProfile_t *pidProfile) {
    uint16_t dynamicKp;

    uint32_t dynamicFactor = constrain(ABS(rcCommand[axis] << 8) / DYNAMIC_PTERM_STICK_THRESHOLD, 0, 1 << 7);

    dynamicKp = ((pidProfile->P8[axis] << 8) + (pidProfile->P8[axis] * dynamicFactor)) >> 8;

    return dynamicKp;
}

uint16_t getDynamicKi(int axis, const pidProfile_t *pidProfile) {
    uint16_t dynamicKi;
    uint16_t resetRate;

    resetRate = (axis == YAW) ? pidProfile->yawItermIgnoreRate : pidProfile->rollPitchItermIgnoreRate;

    uint32_t dynamicFactor = (1 << 8) - constrain((ABS(gyroADC[axis]) << 6) / resetRate, 0, 1 << 8);

    dynamicKi = (pidProfile->I8[axis] * dynamicFactor) >> 8;

    return dynamicKi;
}

void pidResetErrorGyroState(void)
{
    int axis;

    for (axis = 0; axis < 3; axis++) {
        errorGyroI[axis] = 0;
#ifndef SKIP_PID_LUXFLOAT
        errorGyroIf[axis] = 0.0f;
#endif
    }
}

float getdT (void) {
    static float dT;
    if (!dT) dT = (float)targetPidLooptime * 0.000001f;

    return dT;
}

// calculates strength of horizon leveling; 0 = none, 100 = most leveling
int calcHorizonLevelStrength(uint16_t rxConfigMidrc, int horizonInclFact,
                                                          int horizonSensit)
{
    int horizonLevelStrength, inclinationLevelRatio, factorRatio;
    int currentInclination, cutoffDeciDegrees, sensitFact;

    // get raw stick positions (-500 to 500):
    const int32_t stickPosAil = getRcStickDeflection(FD_ROLL, rxConfigMidrc);
    const int32_t stickPosEle = getRcStickDeflection(FD_PITCH, rxConfigMidrc);

    // 0 at center stick, 500 at max stick deflection:
    const int32_t mostDeflectedPos = MAX(ABS(stickPosAil), ABS(stickPosEle));

    // start with 100 at center stick, 0 at max stick deflection:
    horizonLevelStrength = (500 - mostDeflectedPos) / 5;

    // 0 at level, 900 at vertical, 1800 at inverted (degrees * 10)
    currentInclination = MAX(ABS(attitude.values.roll),
                                                ABS(attitude.values.pitch));

    // 'horizon_incl_fact' configuration setting:
    // 0-99 = range 1 (leveling always active when sticks centered)
    // 100-250 = range 2 (leveling can be totally off when inverted)
    if (horizonInclFact >= 100) {
        // range 2 (leveling can be totally off when inverted)
        if (horizonInclFact < 250) {
            // horizon_incl_fact 100 to 200 => 2700 to 900
            //  (represents where leveling goes to zero):
            cutoffDeciDegrees = (250-horizonInclFact) * 18;
            // inclinationLevelRatio (0 to 100) is smaller (less leveling)
            //  for larger inclinations; 0 at cutoffDeciDegrees value:
            inclinationLevelRatio = constrain(
                              (((cutoffDeciDegrees-currentInclination)*10) /
                                           (cutoffDeciDegrees/10)), 0, 100);
            // apply configured horizon sensitivity:
            if (horizonSensit <= 0) {       // zero means no leveling
                horizonLevelStrength = 0;
            } else {
                // when stick is near center (horizonLevelStrength ~= 100)
                //  H_sensitivity value has little effect,
                // when stick is deflected (horizonLevelStrength near 0)
                //  H_sensitivity value has more effect:
                horizonLevelStrength = constrain(
                                        (((horizonLevelStrength-100) * 100 /
                                             horizonSensit) + 100), 0, 100);
            }
            // apply inclination ratio, which may lower leveling
            //  to zero regardless of stick position:
            horizonLevelStrength =
                         horizonLevelStrength * inclinationLevelRatio / 100;
        }
        else
          horizonLevelStrength = 0;
    } else {       // horizon_incl_fact < 100
        // range 1 (leveling always active when sticks centered)
        if (horizonInclFact > 0) {
            // ratio of 100 to 0 (larger means more leveling):
            factorRatio = 100 - horizonInclFact;
            // inclinationLevelRatio (0 to 100) is smaller (less leveling)
            //  for larger inclinations, goes to 100 at inclination level:
            inclinationLevelRatio =
                  ((1800-currentInclination)/18 * (100-factorRatio)) / 100 +
                                                                factorRatio;
            // apply ratio to configured horizon sensitivity:
            sensitFact = horizonSensit * inclinationLevelRatio / 100;
        }
        else   // horizon_incl_fact=0 for "old" functionality
            sensitFact = horizonSensit;

        if (sensitFact <= 0) {           // zero means no leveling
            horizonLevelStrength = 0;
        } else {
            // when stick is near center (horizonLevelStrength ~= 100)
            //  sensitFact value has little effect,
            // when stick is deflected (horizonLevelStrength near 0)
            //  sensitFact value has more effect:
            horizonLevelStrength = constrain(
                    ((horizonLevelStrength - 100) * 100 / sensitFact + 100),
                                                                    0, 100);
        }
    }
    return horizonLevelStrength;
}

const angle_index_t rcAliasToAngleIndexMap[] = { AI_ROLL, AI_PITCH };

static filterStatePt1_t deltaFilterState[3];
static filterStatePt1_t yawFilterState;

#ifndef SKIP_PID_LUXFLOAT
static void pidLuxFloat(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig,
        uint16_t max_angle_inclination, const rollAndPitchTrims_t *angleTrim, const rxConfig_t *rxConfig)
{
    float RateError, AngleRate, gyroRate;
    float ITerm,PTerm,DTerm;
    static float lastRate[3];
    float delta;
    int axis;
    float horizonLevelStrength = 1;

    float tpaFactor = PIDweight[0] / 100.0f; // tpa is now float

    // Scaling factors for Pids to match rewrite and use same defaults
    static const float luxPTermScale = 1.0f / 128;
    static const float luxITermScale = 1000000.0f / 0x1000000;
    static const float luxDTermScale = (0.000001f * (float)0xFFFF) / 508;

    if (FLIGHT_MODE(HORIZON_MODE)) {
        // (convert 0-100 range to 0.0-1.0 range)
        horizonLevelStrength = (float)calcHorizonLevelStrength(rxConfig->midrc,
            pidProfile->horizon_incl_fact, pidProfile->D8[PIDLEVEL]) / 100.0f;
    }

    // ----------PID controller----------
    for (axis = 0; axis < 3; axis++) {
        uint8_t rate = controlRateConfig->rates[axis];

        if (axis == FD_YAW) {
            // YAW is always gyro-controlled (MAG correction is applied to rcCommand) 100dps to 1100dps max yaw rate
            AngleRate = (float)((rate + 47) * rcCommand[YAW]) / 32.0f;
         } else {
             // ACRO mode, control is GYRO based, direct sticks control is applied to rate PID
             AngleRate = (float)((rate + 27) * rcCommand[axis]) / 16.0f; // 200dps to 1200dps max roll/pitch rate
             if (FLIGHT_MODE(ANGLE_MODE) || FLIGHT_MODE(HORIZON_MODE)) {
                // calculate error angle and limit the angle to the max inclination
#ifdef GPS
                const float errorAngle = (constrain(2 * rcCommand[axis] + GPS_angle[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis]); // 16 bits is ok here
#else
                const float errorAngle = (constrain(2 * rcCommand[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis]); // 16 bits is ok here
#endif
                if (FLIGHT_MODE(ANGLE_MODE)) {
                    // ANGLE mode - control is angle based, so control loop is needed
                    AngleRate = errorAngle * pidProfile->P8[PIDLEVEL] / 16.0f;
                } else {
                    // HORIZON mode - direct sticks control is applied to rate PID
                    // mix up angle error to desired AngleRate to add a little auto-level feel
                    AngleRate += errorAngle * pidProfile->I8[PIDLEVEL] * horizonLevelStrength / 16.0f;
                }
            }
        }

        gyroRate = gyroADCf[axis] / 4.0f; // gyro output scaled to rewrite scale

        // --------low-level gyro-based PID. ----------
        // Used in stand-alone mode for ACRO, controlled by higher level regulators in other modes
        // -----calculate scaled error.AngleRates
        // multiplication of rcCommand corresponds to changing the sticks scaling here
        RateError = AngleRate - gyroRate;

        uint16_t kP = (pidProfile->dynamic_pid) ? getDynamicKp(axis, pidProfile) : pidProfile->P8[axis];

        // -----calculate P component
        if ((IS_RC_MODE_ACTIVE(BOXSUPEREXPO) && axis != YAW) || (axis == YAW && rxConfig->superExpoYawMode == SUPEREXPO_YAW_ALWAYS)) {
            PTerm = (luxPTermScale * kP * tpaFactor) * (AngleRate - gyroRate * calculateExpoPlus(axis, rxConfig));
        } else {
            PTerm = luxPTermScale * RateError * kP * tpaFactor;
        }

        // Constrain YAW by yaw_p_limit value if not servo driven in that case servolimits apply
        if((motorCount >= 4 && pidProfile->yaw_p_limit) && axis == YAW) {
            PTerm = constrainf(PTerm, -pidProfile->yaw_p_limit, pidProfile->yaw_p_limit);
        }

        // -----calculate I component.
        uint16_t kI = (pidProfile->dynamic_pid) ? getDynamicKi(axis, pidProfile) : pidProfile->I8[axis];

        errorGyroIf[axis] = constrainf(kI + luxITermScale * RateError * getdT() * pidProfile->I8[axis], -250.0f, 250.0f);

        // limit maximum integrator value to prevent WindUp - accumulating extreme values when system is saturated.
        // I coefficient (I8) moved before integration to make limiting independent from PID settings
        ITerm = errorGyroIf[axis];

        //-----calculate D-term
        if (axis == YAW) {
            if (pidProfile->yaw_lpf_hz) PTerm = filterApplyPt1(PTerm, &yawFilterState, pidProfile->yaw_lpf_hz, getdT());

            axisPID[axis] = lrintf(PTerm + ITerm);

            if (motorCount >= 4) {
                int16_t yaw_jump_prevention_limit = constrain(YAW_JUMP_PREVENTION_LIMIT_HIGH - (pidProfile->D8[axis] << 3), YAW_JUMP_PREVENTION_LIMIT_LOW, YAW_JUMP_PREVENTION_LIMIT_HIGH);

                // prevent "yaw jump" during yaw correction
                axisPID[YAW] = constrain(axisPID[YAW], -yaw_jump_prevention_limit - ABS(rcCommand[YAW]), yaw_jump_prevention_limit + ABS(rcCommand[YAW]));
            }
        } else {
            delta = -(gyroRate - lastRate[axis]);
            lastRate[axis] = gyroRate;

            // Divide delta by targetLooptime to get differential (ie dr/dt)
            delta *= (1.0f / getdT());

            // Filter delta
            if (pidProfile->dterm_lpf_hz) delta = filterApplyPt1(delta, &deltaFilterState[axis], pidProfile->dterm_lpf_hz, getdT());

            DTerm = constrainf(luxDTermScale * delta * (float)pidProfile->D8[axis] * tpaFactor, -300.0f, 300.0f);

            // -----calculate total PID output
            axisPID[axis] = constrain(lrintf(PTerm + ITerm + DTerm), -1000, 1000);
        }

#ifdef GTUNE
        if (FLIGHT_MODE(GTUNE_MODE) && ARMING_FLAG(ARMED)) {
            calculate_Gtune(axis);
        }
#endif

#ifdef BLACKBOX
        axisPID_P[axis] = PTerm;
        axisPID_I[axis] = ITerm;
        axisPID_D[axis] = DTerm;
#endif
    }
}
#endif

static void pidMultiWiiRewrite(const pidProfile_t *pidProfile, const controlRateConfig_t *controlRateConfig, uint16_t max_angle_inclination,
        const rollAndPitchTrims_t *angleTrim, const rxConfig_t *rxConfig)
{
    int axis;
    int32_t PTerm, ITerm, DTerm, delta;
    static int32_t lastRate[3];
    int32_t AngleRateTmp, RateError, gyroRate;

    int horizonLevelStrength = 100;

    if (FLIGHT_MODE(HORIZON_MODE)) {
        // Using Level D as a Sensitivity for Horizon. 0 more rate to 255 more level.
        // For more rate mode decrease D and slower flips and rolls will be possible
        horizonLevelStrength = calcHorizonLevelStrength(
                             rxConfig->midrc, pidProfile->horizon_incl_fact,
                                                  pidProfile->D8[PIDLEVEL]);
    }

    // ----------PID controller----------
    for (axis = 0; axis < 3; axis++) {
        uint8_t rate = controlRateConfig->rates[axis];

        // -----Get the desired angle rate depending on flight mode
        if (axis == FD_YAW) {
            // YAW is always gyro-controlled (MAG correction is applied to rcCommand)
            AngleRateTmp = ((int32_t)(rate + 47) * rcCommand[YAW]) >> 5;
        } else {
            AngleRateTmp = ((int32_t)(rate + 27) * rcCommand[axis]) >> 4;
            if (FLIGHT_MODE(ANGLE_MODE) || FLIGHT_MODE(HORIZON_MODE)) {
                // calculate error angle and limit the angle to max configured inclination
#ifdef GPS
                const int32_t errorAngle = constrain(2 * rcCommand[axis] + GPS_angle[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis];
#else
                const int32_t errorAngle = constrain(2 * rcCommand[axis], -((int) max_angle_inclination),
                    +max_angle_inclination) - attitude.raw[axis] + angleTrim->raw[axis];
#endif
                if (FLIGHT_MODE(ANGLE_MODE)) {
                    // ANGLE mode - control is angle based, so control loop is needed
                    AngleRateTmp = (errorAngle * pidProfile->P8[PIDLEVEL]) >> 4;
                } else {
                    // HORIZON mode - mix up angle error to desired AngleRateTmp to add a little auto-level feel,
                    // horizonLevelStrength is scaled to the stick input
                    AngleRateTmp += (errorAngle * pidProfile->I8[PIDLEVEL] * horizonLevelStrength / 100) >> 4;
                }
            }
        }

        // --------low-level gyro-based PID. ----------
        // Used in stand-alone mode for ACRO, controlled by higher level regulators in other modes
        // -----calculate scaled error.AngleRates
        // multiplication of rcCommand corresponds to changing the sticks scaling here
        gyroRate = gyroADC[axis] / 4;
        RateError = AngleRateTmp - gyroRate;

        uint16_t kP = (pidProfile->dynamic_pid) ? getDynamicKp(axis, pidProfile) : pidProfile->P8[axis];

        // -----calculate P component
        if ((IS_RC_MODE_ACTIVE(BOXSUPEREXPO) && axis != YAW) || (axis == YAW && rxConfig->superExpoYawMode == SUPEREXPO_YAW_ALWAYS)) {
            PTerm = (kP * PIDweight[axis] / 100) * (AngleRateTmp - (int32_t)(gyroRate * calculateExpoPlus(axis, rxConfig))) >> 7;
        } else {
            PTerm = (RateError * kP * PIDweight[axis] / 100) >> 7;
        }

        // Constrain YAW by yaw_p_limit value if not servo driven in that case servolimits apply
        if((motorCount >= 4 && pidProfile->yaw_p_limit) && axis == YAW) {
            PTerm = constrain(PTerm, -pidProfile->yaw_p_limit, pidProfile->yaw_p_limit);
        }

        // -----calculate I component
        // there should be no division before accumulating the error to integrator, because the precision would be reduced.
        // Precision is critical, as I prevents from long-time drift. Thus, 32 bits integrator is used.
        // Time correction (to avoid different I scaling for different builds based on average cycle time)
        // is normalized to cycle time = 2048.
        uint16_t kI = (pidProfile->dynamic_pid) ? getDynamicKi(axis, pidProfile) : pidProfile->I8[axis];

        errorGyroI[axis] = errorGyroI[axis] + ((RateError * (uint16_t)targetPidLooptime) >> 11) * kI;

        // limit maximum integrator value to prevent WindUp - accumulating extreme values when system is saturated.
        // I coefficient (I8) moved before integration to make limiting independent from PID settings
        errorGyroI[axis] = constrain(errorGyroI[axis], (int32_t) - GYRO_I_MAX << 13, (int32_t) + GYRO_I_MAX << 13);

        ITerm = errorGyroI[axis] >> 13;

        //-----calculate D-term
        if (axis == YAW) {
            if (pidProfile->yaw_lpf_hz) PTerm = filterApplyPt1(PTerm, &yawFilterState, pidProfile->yaw_lpf_hz, getdT());

            axisPID[axis] = PTerm + ITerm;

            if (motorCount >= 4) {
                int16_t yaw_jump_prevention_limit = constrain(YAW_JUMP_PREVENTION_LIMIT_HIGH - (pidProfile->D8[axis] << 3), YAW_JUMP_PREVENTION_LIMIT_LOW, YAW_JUMP_PREVENTION_LIMIT_HIGH);

                // prevent "yaw jump" during yaw correction
                axisPID[YAW] = constrain(axisPID[YAW], -yaw_jump_prevention_limit - ABS(rcCommand[YAW]), yaw_jump_prevention_limit + ABS(rcCommand[YAW]));
            }
        } else {
            delta = -(gyroRate - lastRate[axis]);
            lastRate[axis] = gyroRate;

            // Divide delta by targetLooptime to get differential (ie dr/dt)
            delta = (delta * ((uint16_t) 0xFFFF / ((uint16_t)targetPidLooptime >> 4))) >> 5;

            // Filter delta
            if (pidProfile->dterm_lpf_hz) delta = filterApplyPt1((float)delta, &deltaFilterState[axis], pidProfile->dterm_lpf_hz, getdT());

            DTerm = (delta * pidProfile->D8[axis] * PIDweight[axis] / 100) >> 8;

            // -----calculate total PID output
            axisPID[axis] = PTerm + ITerm + DTerm;
        }


#ifdef GTUNE
        if (FLIGHT_MODE(GTUNE_MODE) && ARMING_FLAG(ARMED)) {
             calculate_Gtune(axis);
        }
#endif

#ifdef BLACKBOX
        axisPID_P[axis] = PTerm;
        axisPID_I[axis] = ITerm;
        axisPID_D[axis] = DTerm;
#endif
    }
}

void pidSetController(pidControllerType_e type)
{
    switch (type) {
        default:
        case PID_CONTROLLER_MWREWRITE:
            pid_controller = pidMultiWiiRewrite;
            break;
#ifndef SKIP_PID_LUXFLOAT
        case PID_CONTROLLER_LUX_FLOAT:
            pid_controller = pidLuxFloat;
#endif
    }
}

