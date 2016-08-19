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

#include "platform.h"

#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "drivers/sensor.h"
#include "drivers/system.h"
#include "drivers/accgyro.h"
#include "drivers/accgyro_mpu.h"

#include "io/beeper.h"
#include "io/statusindicator.h"

#include "sensors/sensors.h"
#include "sensors/boardalignment.h"
#include "sensors/gyro.h"

#include "flight/pid.h"

#include "acceleration.h"

gyro_t gyro;                      // gyro access functions
sensor_align_e gyroAlign = 0;

int32_t gyroADC[XYZ_AXIS_COUNT];
float gyroADCf[XYZ_AXIS_COUNT];

static int32_t gyroZero[XYZ_AXIS_COUNT] = { 0, 0, 0 };
static const gyroConfig_t *gyroConfig;
static biquadFilter_t gyroFilterLPF[XYZ_AXIS_COUNT];
static biquadFilter_t gyroFilterNotch[XYZ_AXIS_COUNT];
static pt1Filter_t gyroFilterPt1[XYZ_AXIS_COUNT];
static uint8_t gyroSoftLpfType;
static uint16_t gyroSoftNotchHz;
static float gyroSoftNotchQ;
static uint8_t gyroSoftLpfHz;
static uint16_t calibratingG = 0;
static float gyroDt;
uint32_t lastGyroInterruptCallDelta;
static uint8_t pidProcessDenom;
static bool isPidScheduledToRun;
static uint8_t accDividerDrops;

void gyroUseConfig(const gyroConfig_t *gyroConfigToUse,
        uint8_t gyro_soft_lpf_hz,
        uint16_t gyro_soft_notch_hz,
        uint16_t gyro_soft_notch_cutoff,
        uint8_t gyro_soft_lpf_type,
        uint8_t pid_process_denom)
{
    gyroConfig = gyroConfigToUse;
    pidProcessDenom = pid_process_denom;
    gyroSoftLpfHz = gyro_soft_lpf_hz;
    gyroSoftNotchHz = gyro_soft_notch_hz;
    gyroSoftLpfType = gyro_soft_lpf_type;
    gyroSoftNotchQ = filterGetNotchQ(gyro_soft_notch_hz, gyro_soft_notch_cutoff);
}

void gyroInit(void)
{
    if (gyroSoftLpfHz && gyro.gyroSamplingInterval) {  // Initialisation needs to happen once samplingrate is known
        for (int axis = 0; axis < 3; axis++) {
            biquadFilterInit(&gyroFilterNotch[axis], gyroSoftNotchHz, gyro.gyroSamplingInterval, gyroSoftNotchQ, FILTER_NOTCH);
            if (gyroSoftLpfType == FILTER_BIQUAD)
                biquadFilterInitLPF(&gyroFilterLPF[axis], gyroSoftLpfHz, gyro.gyroSamplingInterval);
            else
                gyroDt = (float) gyro.gyroSamplingInterval * 0.000001f;
        }
    }
}

void setAccDividerDrops(bool accEnabled) {
    if (accEnabled){
        if (gyro.gyroSamplingInterval < INTERVAL_1KHZ)
            accDividerDrops = INTERVAL_1KHZ / gyro.gyroSamplingInterval;
        else
            accDividerDrops = 1;
    }
}

bool isGyroCalibrationComplete(void)
{
    return calibratingG == 0;
}

static bool isOnFinalGyroCalibrationCycle(void)
{
    return calibratingG == 1;
}

static uint16_t gyroCalculateCalibratingCycles(void)
{
    return (CALIBRATING_GYRO_CYCLES / targetPidLooptime) * CALIBRATING_GYRO_CYCLES;
}

static bool isOnFirstGyroCalibrationCycle(void)
{
    return calibratingG == gyroCalculateCalibratingCycles();
}

void gyroSetCalibrationCycles(void)
{
    calibratingG = gyroCalculateCalibratingCycles();
}

static void performGyroCalibration(uint8_t gyroMovementCalibrationThreshold)
{
    static int32_t g[3];
    static stdev_t var[3];

    for (int axis = 0; axis < 3; axis++) {

        // Reset g[axis] at start of calibration
        if (isOnFirstGyroCalibrationCycle()) {
            g[axis] = 0;
            devClear(&var[axis]);
        }

        // Sum up CALIBRATING_GYRO_CYCLES readings
        g[axis] += gyroADC[axis];
        devPush(&var[axis], gyroADC[axis]);

        // Reset global variables to prevent other code from using un-calibrated data
        gyroADC[axis] = 0;
        gyroZero[axis] = 0;

        if (isOnFinalGyroCalibrationCycle()) {
            float dev = devStandardDeviation(&var[axis]);
            // check deviation and startover in case the model was moved
            if (gyroMovementCalibrationThreshold && dev > gyroMovementCalibrationThreshold) {
                gyroSetCalibrationCycles();
                return;
            }
            gyroZero[axis] = (g[axis] + (gyroCalculateCalibratingCycles() / 2)) / gyroCalculateCalibratingCycles();
        }
    }

    if (isOnFinalGyroCalibrationCycle()) {
        beeper(BEEPER_GYRO_CALIBRATED);
    }
    calibratingG--;

}

static void applyGyroZero(void)
{
    for (int axis = 0; axis < 3; axis++) {
        gyroADC[axis] -= gyroZero[axis];
    }
}

void gyroHandleInterrupt(void) {
    static uint32_t lastGyroInterruptCallAt = 0;
    uint32_t now = micros();
    lastGyroInterruptCallDelta = now - lastGyroInterruptCallAt;
    debug[0] = lastGyroInterruptCallDelta;
    lastGyroInterruptCallAt = now;
    static int accReadCountDown;

	if (gyro.gyroSamplingEnabled) {
        static int16_t gyroADCRaw[XYZ_AXIS_COUNT];
        static int pidProcessCountDown;

        if (accDividerDrops) {
            if (accReadCountDown) {
                accReadCountDown--;
                if (!gyro.read(gyroADCRaw)) return;
            } else {
                accReadCountDown = accDividerDrops - 1;
                if (!acc.read(gyroADCRaw, accADCRaw)) return;
            }
	    } else {
	        if (!gyro.read(gyroADCRaw)) return;
	    }

        processGyroData(gyroADCRaw);

        if (pidProcessCountDown) {
            pidProcessCountDown--;
        } else {
            pidProcessCountDown = pidProcessDenom - 1;
            isPidScheduledToRun = true;
        }
	}
}

void processGyroData(int16_t *gyroADCRaw) {

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        if (debugMode == DEBUG_GYRO) debug[axis] = gyroADC[axis];
        gyroADC[axis] = gyroADCRaw[axis];
    }

    alignSensors(gyroADC, gyroADC, gyroAlign);

    if (!isGyroCalibrationComplete()) {
        performGyroCalibration(gyroConfig->gyroMovementCalibrationThreshold);
    }

    applyGyroZero();

    if (gyroSoftLpfHz) {
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            float sample = (float) gyroADC[axis];
            if (gyroSoftNotchHz) {
                sample = biquadFilterApply(&gyroFilterNotch[axis], sample);
            }

            if (debugMode == DEBUG_NOTCH && axis < 2){
                debug[axis*2 + 0] = gyroADC[axis];
                debug[axis*2 + 1] = lrintf(sample);
            }

            if (gyroSoftLpfType == FILTER_BIQUAD) {
                gyroADCf[axis] = biquadFilterApply(&gyroFilterLPF[axis], sample);
            } else {
                gyroADCf[axis] = pt1FilterApply4(&gyroFilterPt1[axis], sample, gyroSoftLpfHz, gyroDt);
            }
            gyroADC[axis] = lrintf(gyroADCf[axis]);
        }
    } else {
        for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
            gyroADCf[axis] = gyroADC[axis];
        }
    }
}

bool pidScheduledToRun(void)
{
    bool ret;
    if (isPidScheduledToRun) {
        ret = true;
        isPidScheduledToRun= false;
    } else {
        ret = false;
    }
    return ret;
}

