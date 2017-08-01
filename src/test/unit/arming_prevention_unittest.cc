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

#include <stdint.h>

extern "C" {
    #include "blackbox/blackbox.h"
    #include "build/debug.h"
    #include "common/maths.h"
    #include "config/parameter_group.h"
    #include "config/parameter_group_ids.h"
    #include "fc/config.h"
    #include "fc/controlrate_profile.h"
    #include "fc/fc_core.h"
    #include "fc/rc_controls.h"
    #include "fc/rc_modes.h"
    #include "fc/runtime_config.h"
    #include "flight/imu.h"
    #include "flight/mixer.h"
    #include "flight/pid.h"
    #include "flight/servos.h"
    #include "io/beeper.h"
    #include "io/gps.h"
    #include "rx/rx.h"
    #include "scheduler/scheduler.h"
    #include "sensors/acceleration.h"
    #include "sensors/gyro.h"
    #include "telemetry/telemetry.h"

    PG_REGISTER(accelerometerConfig_t, accelerometerConfig, PG_ACCELEROMETER_CONFIG, 0);
    PG_REGISTER(blackboxConfig_t, blackboxConfig, PG_BLACKBOX_CONFIG, 0);
    PG_REGISTER(gyroConfig_t, gyroConfig, PG_GYRO_CONFIG, 0);
    PG_REGISTER(mixerConfig_t, mixerConfig, PG_MIXER_CONFIG, 0);
    PG_REGISTER(pidConfig_t, pidConfig, PG_PID_CONFIG, 0);
    PG_REGISTER(rxConfig_t, rxConfig, PG_RX_CONFIG, 0);
    PG_REGISTER(servoConfig_t, servoConfig, PG_SERVO_CONFIG, 0);
    PG_REGISTER(systemConfig_t, systemConfig, PG_SYSTEM_CONFIG, 0);
    PG_REGISTER(telemetryConfig_t, telemetryConfig, PG_TELEMETRY_CONFIG, 0);

    float rcCommand[4];
    int16_t rcData[MAX_SUPPORTED_RC_CHANNEL_COUNT];
    uint16_t averageSystemLoadPercent = 0;
    uint8_t cliMode = 0;
    uint8_t debugMode = 0;
    int16_t debug[DEBUG16_VALUE_COUNT];
    pidProfile_t *currentPidProfile;
    controlRateConfig_t *currentControlRateProfile;
    attitudeEulerAngles_t attitude;
    gpsSolutionData_t gpsSol;
    uint32_t targetPidLooptime;
    bool cmsInMenu = false;
}

uint32_t simulationTime = 0;

#include "gtest/gtest.h"

TEST(ArmingPreventionTest, AngleThrottleArmguard)
{
    // given
    simulationTime = 0;

    // and
    modeActivationConditionsMutable(0)->auxChannelIndex = 0;
    modeActivationConditionsMutable(0)->modeId = BOXARM;
    modeActivationConditionsMutable(0)->range.startStep = CHANNEL_VALUE_TO_STEP(1750);
    modeActivationConditionsMutable(0)->range.endStep = CHANNEL_VALUE_TO_STEP(CHANNEL_RANGE_MAX);
    useRcControlsConfig(NULL);

    // and
    rxConfigMutable()->mincheck = 1050;
    systemConfigMutable()->powerOnArmGuardTime = 5;

    // and
    // arming initially disabled by guard
    setArmingDisabled(ARMING_DISABLED_POWER_ON_GUARD);
    resetPowerOnGuardTime();

    // and
    // default channel positions
    rcData[THROTTLE] = 1400;
    rcData[4] = 1800;

    // when
    updateActivatedModes();
    updateArmingStatus();

    // expect
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_ANGLE | ARMING_DISABLED_POWER_ON_GUARD | ARMING_DISABLED_THROTTLE, getArmingDisableFlags());

    // given
    // quad is level
    ENABLE_STATE(SMALL_ANGLE);

    // when
    updateArmingStatus();

    // expect
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_POWER_ON_GUARD | ARMING_DISABLED_THROTTLE, getArmingDisableFlags());

    // given
    rcData[THROTTLE] = 1000;

    // when
    updateArmingStatus();

    // expect
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_POWER_ON_GUARD, getArmingDisableFlags());

    // when
    // arm guard time elapses
    for (int i = 0; i < systemConfig()->powerOnArmGuardTime; i++) {
        simulationTime += 1e6;
        updateActivatedModes();
        updateArmingStatus();
    }

    // expect
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_POWER_ON_GUARD, getArmingDisableFlags());

    // given
    rcData[4] = 1000;

    // when
    // arm guard time elapses
    for (int i = 0; i < systemConfig()->powerOnArmGuardTime; i++) {
        simulationTime += 1e6;
        updateActivatedModes();
        updateArmingStatus();
    }

    // expect
    EXPECT_EQ(0, getArmingDisableFlags());
    EXPECT_FALSE(isArmingDisabled());
}

TEST(ArmingPreventionTest, ArmingGuardRadioLeftOnAndArmed)
{
    // given
    simulationTime = 0;

    // and
    modeActivationConditionsMutable(0)->auxChannelIndex = 0;
    modeActivationConditionsMutable(0)->modeId = BOXARM;
    modeActivationConditionsMutable(0)->range.startStep = CHANNEL_VALUE_TO_STEP(1750);
    modeActivationConditionsMutable(0)->range.endStep = CHANNEL_VALUE_TO_STEP(CHANNEL_RANGE_MAX);
    useRcControlsConfig(NULL);

    // and
    rxConfigMutable()->mincheck = 1050;
    systemConfigMutable()->powerOnArmGuardTime = 5;

    // and
    // arming initially disabled by guard
    setArmingDisabled(ARMING_DISABLED_POWER_ON_GUARD);
    resetPowerOnGuardTime();

    // given
    rcData[THROTTLE] = 1000;
    ENABLE_STATE(SMALL_ANGLE);

    // when
    updateActivatedModes();
    updateArmingStatus();

    // expect
    EXPECT_FALSE(isUsingSticksForArming());
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_POWER_ON_GUARD, getArmingDisableFlags());

    // given
    // arm channel takes a safe default value from the RX after power on
    rcData[4] = 1500;

    // and
    // a short time passes before the RF link is made
    simulationTime += 1e6;

    // when
    updateActivatedModes();
    updateArmingStatus();

    // expect
    // arming should still be disabled as the timeout has yet to elapse
    EXPECT_TRUE(isArmingDisabled());
    EXPECT_EQ(ARMING_DISABLED_POWER_ON_GUARD, getArmingDisableFlags());

    // given
    // arm switch is switched off by user
    rcData[4] = 1000;

    // and
    simulationTime += 1e6 * systemConfig()->powerOnArmGuardTime;

    // when
    updateActivatedModes();
    updateArmingStatus();

    // expect
    // arming enabled as arm switch has been off for sufficient time
    EXPECT_EQ(0, getArmingDisableFlags());
    EXPECT_FALSE(isArmingDisabled());
}

// STUBS
extern "C" {
    uint32_t micros(void) { return simulationTime; }
    uint32_t millis(void) { return micros() / 1000; }
    bool rxIsReceivingSignal(void) { return false; }

    bool feature(uint32_t) { return false; }
    void warningLedFlash(void) {}
    void warningLedDisable(void) {}
    void warningLedUpdate(void) {}
    void beeper(beeperMode_e) {}
    void beeperConfirmationBeeps(uint8_t) {}
    void beeperWarningBeeps(uint8_t) {}
    void beeperSilence(void) {}
    void systemBeep(bool) {}
    void saveConfigAndNotify(void) {}
    void blackboxFinish(void) {}
    bool isAccelerationCalibrationComplete(void) { return true; }
    bool isBaroCalibrationComplete(void) { return true; }
    bool isGyroCalibrationComplete(void) { return true; }
    void gyroStartCalibration(bool) {}
    bool isFirstArmingGyroCalibrationRunning(void) { return false; }
    void pidController(const pidProfile_t *, const rollAndPitchTrims_t *, timeUs_t) {}
    void pidStabilisationState(pidStabilisationState_e) {}
    void mixTable(uint8_t) {};
    void writeMotors(void) {};
    void writeServos(void) {};
    void calculateRxChannelsAndUpdateFailsafe(timeUs_t) {}
    bool isMixerUsingServos(void) { return false; }
    void gyroUpdate(void) {}
    timeDelta_t getTaskDeltaTime(cfTaskId_e) { return 0; }
    void updateRSSI(timeUs_t) {}
    bool failsafeIsMonitoring(void) { return false; }
    void failsafeStartMonitoring(void) {}
    void failsafeUpdateState(void) {}
    bool failsafeIsActive(void) { return false; }
    void pidResetErrorGyroState(void) {}
    void updateAdjustmentStates(void) {}
    void processRcAdjustments(controlRateConfig_t *) {}
    void updateGpsWaypointsAndMode(void) {}
    void releaseSharedTelemetryPorts(void) {}
    void telemetryCheckState(void) {}
    void mspSerialAllocatePorts(void) {}
    void gyroReadTemperature(void) {}
    void updateRcCommands(void) {}
    void applyAltHold(void) {}
    void resetYawAxis(void) {}
    int16_t calculateThrottleAngleCorrection(uint8_t) { return 0; }
    void processRcCommand(void) {}
    void updateGpsStateForHomeAndHoldMode(void) {}
    void blackboxUpdate(timeUs_t) {}
    void transponderUpdate(timeUs_t) {}
    void GPS_reset_home_position(void) {}
    void accSetCalibrationCycles(uint16_t) {}
    void baroSetCalibrationCycles(uint16_t) {}
    void changePidProfile(uint8_t) {}
    void dashboardEnablePageCycling(void) {}
    void dashboardDisablePageCycling(void) {}
}
