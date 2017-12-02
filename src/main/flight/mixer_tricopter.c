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
#include <float.h>

#define MIXER_TRICOPTER_INTERNALS

#include "platform.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "common/maths.h"
#include "common/axis.h"
#include "common/filter.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/sensor.h"
#include "drivers/system.h"
#include "drivers/adc.h"
#include "drivers/time.h"

#include "rx/rx.h"

#include "io/beeper.h"
#include "io/motors.h"
#include "io/servos.h"

#include "sensors/sensors.h"
#include "sensors/acceleration.h"
#include "sensors/gyro.h"

#include "flight/mixer.h"
#include "flight/mixer_tricopter.h"
#include "flight/imu.h"
#include "flight/pid.h"

#include "fc/fc_rc.h"
#include "fc/config.h"
#include "fc/runtime_config.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"

#define TRI_TAIL_SERVO_ANGLE_MID                (900)
#define TRI_YAW_FORCE_CURVE_SIZE                (100)
#define TRI_TAIL_SERVO_MAX_ANGLE                (500)
#define TRI_SERVO_SATURATION_DPS_ERROR_LIMIT    (75.0f)

static const uint8_t TRI_TAIL_MOTOR_INDEX = 0;
static const int32_t TRI_YAW_FORCE_PRECISION = 1000;

// Use the first once at the top of every function that will use one of the other
#define InitDelayMeasurement_ms() const uint32_t now_ms = millis()
#define IsDelayElapsed_ms(timestamp_ms, delay_ms) ((uint32_t) (now_ms - timestamp_ms) >= delay_ms)
#define GetCurrentDelay_ms(timestamp_ms) (now_ms - timestamp_ms)
#define GetCurrentTime_ms() (now_ms)

static tailTune_t tailTune = { .mode = TT_MODE_NONE };
static tailServo_t tailServo = { .angle = TRI_TAIL_SERVO_ANGLE_MID, .ADCChannel = ADC_RSSI };
static int32_t yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE];
static tailMotor_t tailMotor = { .accelerationDelay_ms = 30, .decelerationDelay_ms = 100, .virtualFeedBack = 1000.0f };
// Configured output throttle range (max - min)
static int16_t throttleRange = 0;
static float throttleHalfRange;
static float throttleMidPoint;
static float pitchCorrectionGain;
static float dynamicYawGainAtMax;
static triMixerConfig_t *gpTriMixerConfig;
static int16_t lastMotorCorrection = 0;

static void initYawForceCurve(void);
static uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle);
static float getPitchCorrectionAtTailAngle(float angle, float thrustFactor);
static uint16_t getAngleFromYawForceCurve(int32_t force);
static uint16_t getServoAngle(servoParam_t *servoConf, uint16_t servoValue);
static uint16_t getLinearServoValue(servoParam_t *servoConf, float scaledPIDOutput, float pidSumLimit);
static uint16_t getNormalServoValue(servoParam_t *servoConf, float constrainedPIDOutput, float pidSumLimit);
static uint16_t virtualServoStep(uint16_t currentAngle, int16_t servoSpeed, float dT, servoParam_t *servoConf,
        uint16_t servoValue);
static uint16_t feedbackServoStep(triMixerConfig_t *mixerConf, uint16_t tailServoADC);
STATIC_UNIT_TESTED void tailTuneModeThrustTorque(thrustTorque_t *pTT, const bool isThrottleHigh);
static void tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal);
static void triTailTuneStep(servoParam_t *pServoConf, int16_t *pServoVal);
static void updateServoAngle(float dT);
static AdcChannel getServoFeedbackADCChannel(uint8_t tri_servo_feedback);
static void predictGyroOnDeceleration(void);
static float scalePIDBasedOnTailMotorSpeed(float scaledPidOutput, float pidSumLimit);
static void tailMotorStep(int16_t setpoint, float dT);
static int8_t triGetServoDirection(void);

PG_REGISTER_WITH_RESET_FN(triMixerConfig_t, triMixerConfig, PG_TRICOPTER_CONFIG, 0);
#include "target.h"

#ifndef DEFAULT_SERVO_FEEDBACK_SOURCE
#define DEFAULT_SERVO_FEEDBACK_SOURCE TRI_SERVO_FB_VIRTUAL
#endif

void pgResetFn_triMixerConfig(triMixerConfig_t *triMixerConfig)
{
    triMixerConfig->tri_motor_acc_yaw_correction = 27;
    triMixerConfig->tri_motor_acceleration = 18;
    triMixerConfig->tri_servo_feedback = DEFAULT_SERVO_FEEDBACK_SOURCE;
    triMixerConfig->tri_servo_max_adc = 0;
    triMixerConfig->tri_servo_mid_adc = 0;
    triMixerConfig->tri_servo_min_adc = 0;
    triMixerConfig->tri_tail_motor_thrustfactor = 138;
    triMixerConfig->tri_tail_servo_speed = 300; // Default for BMS-210DMH at 5V
    triMixerConfig->tri_yaw_boost = 300;
    triMixerConfig->tri_dynamic_yaw_maxthrottle = 38;
    triMixerConfig->tri_servo_angle_at_max = 40;
}

void triInitMixer(servoParam_t *pTailServoConfig, int16_t *pTailServoOutput)
{
    tailServo.pConf = pTailServoConfig;
    tailServo.pOutput = pTailServoOutput;
    gpTriMixerConfig = triMixerConfigMutable();
    tailServo.thrustFactor = gpTriMixerConfig->tri_tail_motor_thrustfactor / 10.0f;
    tailServo.maxDeflection = gpTriMixerConfig->tri_servo_angle_at_max * 10;
    tailServo.angleAtMin = TRI_TAIL_SERVO_ANGLE_MID - tailServo.maxDeflection;
    tailServo.angleAtMax = TRI_TAIL_SERVO_ANGLE_MID + tailServo.maxDeflection;
    tailServo.speed = gpTriMixerConfig->tri_tail_servo_speed;
    tailServo.saturated = false;
    throttleRange = motorConfig()->maxthrottle - motorConfig()->minthrottle;
    throttleHalfRange = throttleRange / 2.0f;
    throttleMidPoint = motorConfig()->minthrottle + throttleHalfRange;
    pitchCorrectionGain = gpTriMixerConfig->tri_yaw_boost / 100.0f;
    dynamicYawGainAtMax = gpTriMixerConfig->tri_dynamic_yaw_maxthrottle / 100.0f;
    const float tri_motor_acceleration_float = (float)gpTriMixerConfig->tri_motor_acceleration / 100;
    tailMotor.acceleration = (float) throttleRange / tri_motor_acceleration_float;
    initYawForceCurve();
    tailServo.ADCChannel = getServoFeedbackADCChannel(gpTriMixerConfig->tri_servo_feedback);
}

static void initYawForceCurve(void)
{
    // DERIVATE(1/(sin(x)-cos(x)/tailServoThrustFactor)) = 0
    // Multiplied by 10 to get decidegrees
    const int16_t minAngle = tailServo.angleAtMin;
    const int16_t maxAngle = tailServo.angleAtMax;
    int32_t maxNegForce = 0;
    int32_t maxPosForce = 0;

    tailMotor.pitchZeroAngle = 10.0f * 2.0f
            * atanf((sqrtf(tailServo.thrustFactor * tailServo.thrustFactor + 1) + 1) / tailServo.thrustFactor);
    tailMotor.accelerationDelay_angle = 10.0f * (tailMotor.accelerationDelay_ms / 1000.0f) * tailServo.speed;
    tailMotor.decelerationDelay_angle = 10.0f * (tailMotor.decelerationDelay_ms / 1000.0f) * tailServo.speed;

    int16_t angle = TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    for (int32_t i = 0; i < TRI_YAW_FORCE_CURVE_SIZE; i++) {
        const float angleRad = DEGREES_TO_RADIANS(angle / 10.0f);

        yawForceCurve[i] = TRI_YAW_FORCE_PRECISION * (-tailServo.thrustFactor * cosf(angleRad) - sinf(angleRad))
                * getPitchCorrectionAtTailAngle(angleRad, tailServo.thrustFactor);
        // Only calculate the top forces in the configured angle range
        if ((angle >= minAngle) && (angle <= maxAngle)) {
            maxNegForce = MIN(yawForceCurve[i], maxNegForce);
            maxPosForce = MAX(yawForceCurve[i], maxPosForce);
        }
        angle += 10;
    }
    tailServo.maxYawForce = MIN(ABS(maxNegForce), ABS(maxPosForce));
}

uint16_t triGetCurrentServoAngle(void)
{
    return tailServo.angle;
}

static uint16_t getLinearServoValue(servoParam_t *servoConf, float scaledPIDOutput, float pidSumLimit)
{
    const int32_t linearYawForceAtValue = tailServo.maxYawForce * scaledPIDOutput / pidSumLimit;
    const int16_t correctedAngle = getAngleFromYawForceCurve(linearYawForceAtValue);
    const uint16_t linearServoValue = getServoValueAtAngle(servoConf, correctedAngle);

    return linearServoValue;
}

static uint16_t getNormalServoValue(servoParam_t *servoConf, float constrainedPIDOutput, float pidSumLimit)
{
    const int16_t angle = TRI_TAIL_SERVO_ANGLE_MID + constrainedPIDOutput / pidSumLimit * tailServo.maxDeflection;
    const uint16_t normalServoValue = getServoValueAtAngle(servoConf, angle);

    return normalServoValue;
}

void triServoMixer(float scaledYawPid, float pidSumLimit)
{
    const float dT = getdT();

    // Update the tail motor speed from feedback
    tailMotorStep(motor[TRI_TAIL_MOTOR_INDEX], dT);

    // Update the servo angle from feedback
    updateServoAngle(dT);

    // Correct the yaw PID output based on tail motor speed
    scaledYawPid = scalePIDBasedOnTailMotorSpeed(scaledYawPid, pidSumLimit);

    // Correct the servo output to produce linear yaw thrust in armed state
    if (ARMING_FLAG(ARMED)) {
        *tailServo.pOutput = getLinearServoValue(tailServo.pConf, scaledYawPid, pidSumLimit);
    } else {
        *tailServo.pOutput = getNormalServoValue(tailServo.pConf, scaledYawPid, pidSumLimit);
    }

    // Run tail tune mode
    triTailTuneStep(tailServo.pConf, tailServo.pOutput);

    // Check for tail motor decelaration and determine expected produced yaw error
    predictGyroOnDeceleration();
}

int16_t triGetMotorCorrection(uint8_t motorIndex)
{
    uint16_t correction = 0;

    if (motorIndex == TRI_TAIL_MOTOR_INDEX) {
        // TODO: revise this function, is the speed up lag really needed? Test without.

        // Adjust tail motor speed based on servo angle. Check how much to adjust speed from pitch force curve based on servo angle.
        // Take motor speed up lag into account by shifting the phase of the curve
        // Not taking into account the motor braking lag (yet)
        const uint16_t servoAngle = triGetCurrentServoAngle();
        correction = (throttleRange * getPitchCorrectionAtTailAngle(DEGREES_TO_RADIANS(servoAngle / 10.0f), tailServo.thrustFactor)) - throttleRange;

        // Multiply the correction to get more authority
        correction *= pitchCorrectionGain;
        lastMotorCorrection = correction;
    }

    return correction;
}

_Bool triIsEnabledServoUnarmed(void)
{
    const _Bool isEnabledServoUnarmed = (gpTriMixerConfig->tri_unarmed_servo != 0) || FLIGHT_MODE(TAILTUNE_MODE);

    return isEnabledServoUnarmed;
}

_Bool triMixerInUse(void)
{
    const _Bool isEnabledServoUnarmed = ((mixerConfig()->mixerMode == MIXER_TRI) || (mixerConfig()->mixerMode == MIXER_CUSTOM_TRI));

    return isEnabledServoUnarmed;
}

_Bool triIsServoSaturated(float rateError)
{
    if (fabsf(rateError) > 75.0f) {
        return true;
    } else {
        return false;
    }
}

static uint16_t getServoValueAtAngle(servoParam_t *servoConf, uint16_t angle)
{
    const int16_t servoMid = servoConf->middle;
    uint16_t servoValue;

    if (angle == TRI_TAIL_SERVO_ANGLE_MID) {
        servoValue = servoMid;
    } else {
        const int8_t direction = triGetServoDirection();
        const uint16_t angleRange = tailServo.maxDeflection;
        uint16_t angleDiff;
        int8_t servoRange; // -1 == min-mid, 1 == mid-max

        if (angle < TRI_TAIL_SERVO_ANGLE_MID) {
            angleDiff = TRI_TAIL_SERVO_ANGLE_MID - angle;
            if (direction > 0) {
                servoRange = -1;
            } else {
                servoRange = 1;
            }
        } else {
            angleDiff = angle - TRI_TAIL_SERVO_ANGLE_MID;
            if (direction > 0) {
                servoRange = 1;
            } else {
                servoRange = -1;
            }
        }
        if (servoRange < 0) {
            const int16_t servoMin = servoConf->min;

            servoValue = servoMid - angleDiff * (servoMid - servoMin) / angleRange;
        } else {
            const int16_t servoMax = servoConf->max;

            servoValue = servoMid + angleDiff * (servoMax - servoMid) / angleRange;
        }
    }

    return servoValue;
}

static float getPitchCorrectionAtTailAngle(float angle, float thrustFactor)
{
    const float pitchCorrection = 1.0f / (sin_approx(angle) - cos_approx(angle) / thrustFactor);

    return pitchCorrection;
}

static uint16_t getAngleFromYawForceCurve(int32_t force)
{
    uint16_t angle;

    if (force < yawForceCurve[0]) {
        // No force that low
        angle = TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE;
    } else if (!(force < yawForceCurve[TRI_YAW_FORCE_CURVE_SIZE - 1])) {
        // No force that high
        angle = TRI_TAIL_SERVO_ANGLE_MID + TRI_TAIL_SERVO_MAX_ANGLE;
    } else {
        // Binary search: yawForceCurve[lower] <= force, yawForceCurve[higher] > force
        int32_t lower = 0;
        int32_t higher = TRI_YAW_FORCE_CURVE_SIZE - 1;

        while (higher > lower + 1) {
            const int32_t mid = (lower + higher) / 2;
            if (yawForceCurve[mid] > force) {
                higher = mid;
            } else {
                lower = mid;
            }
        }
        // Interpolating
        angle = TRI_TAIL_SERVO_ANGLE_MID - TRI_TAIL_SERVO_MAX_ANGLE + lower * 10
                + (int32_t)(force - yawForceCurve[lower]) * 10 / (yawForceCurve[higher] - yawForceCurve[lower]);
    }

    return angle;
}

static uint16_t getServoAngle(servoParam_t *servoConf, uint16_t servoValue)
{
    const int16_t midValue = servoConf->middle;
    const int16_t endValue = servoValue < midValue ? servoConf->min : servoConf->max;
    const int16_t endAngle = servoValue < midValue ? tailServo.angleAtMin : tailServo.angleAtMax;
    const int16_t servoAngle = (int32_t)(endAngle - TRI_TAIL_SERVO_ANGLE_MID) * (servoValue - midValue)
            / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID;

    return servoAngle;
}

static uint16_t virtualServoStep(uint16_t currentAngle, int16_t servoSpeed, float dT, servoParam_t *servoConf,
        uint16_t servoValue)
{
    const uint16_t angleSetPoint = getServoAngle(servoConf, servoValue);
    const uint16_t dA = dT * servoSpeed * 10; // Max change of an angle since last check

    if (ABS(currentAngle - angleSetPoint) < dA) {
        // At set-point after this moment
        currentAngle = angleSetPoint;
    } else if (currentAngle < angleSetPoint) {
        currentAngle += dA;
    } else {
        // tailServoAngle.virtual > angleSetPoint
        currentAngle -= dA;
    }

    return currentAngle;
}

static uint16_t feedbackServoStep(triMixerConfig_t *mixerConf, uint16_t tailServoADC)
{
    // Feedback servo
    const int32_t ADCFeedback = tailServoADC;
    const int16_t midValue = mixerConf->tri_servo_mid_adc;
    const int16_t endValue = ADCFeedback < midValue ? mixerConf->tri_servo_min_adc : mixerConf->tri_servo_max_adc;
    const int16_t tailServoMaxAngle = tailServo.angleAtMax;
    const int16_t endAngle = ADCFeedback < midValue ?
            TRI_TAIL_SERVO_ANGLE_MID - tailServoMaxAngle : TRI_TAIL_SERVO_ANGLE_MID + tailServoMaxAngle;
    const int16_t currentAngle = ((endAngle - TRI_TAIL_SERVO_ANGLE_MID) * (ADCFeedback - midValue)
            / (endValue - midValue) + TRI_TAIL_SERVO_ANGLE_MID);

    return currentAngle;
}

static void triTailTuneStep(servoParam_t *pServoConf, int16_t *pServoVal)
{
    if (!IS_RC_MODE_ACTIVE(BOXTAILTUNE)) {
        if (FLIGHT_MODE(TAILTUNE_MODE)) {
            DISABLE_ARMING_FLAG(ARMING_DISABLED_TAIL_TUNE);
            DISABLE_FLIGHT_MODE(TAILTUNE_MODE);
            tailTune.mode = TT_MODE_NONE;
        }
    } else {
        ENABLE_FLIGHT_MODE(TAILTUNE_MODE);
        if (tailTune.mode == TT_MODE_NONE) {
            if (ARMING_FLAG(ARMED)) {
                tailTune.mode = TT_MODE_THRUST_TORQUE;
                tailTune.tt.state = TT_IDLE;
            } else {
                // Prevent accidental arming in servo setup mode
                ENABLE_ARMING_FLAG(ARMING_DISABLED_TAIL_TUNE);
                tailTune.mode = TT_MODE_SERVO_SETUP;
                tailTune.ss.servoVal = pServoConf->middle;
            }
        }
        switch (tailTune.mode) {
        case TT_MODE_THRUST_TORQUE:
            tailTuneModeThrustTorque(&tailTune.tt,
                    (THROTTLE_HIGH == calculateThrottleStatus()));
            break;
        case TT_MODE_SERVO_SETUP:
            tailTuneModeServoSetup(&tailTune.ss, pServoConf, pServoVal);
            break;
        case TT_MODE_NONE:
            break;
        }
    }
}

STATIC_UNIT_TESTED void tailTuneModeThrustTorque(thrustTorque_t *pTT, const bool isThrottleHigh)
{
    InitDelayMeasurement_ms();
    switch (pTT->state) {
    case TT_IDLE:
        // Calibration has been requested, only start when throttle is up
        if (isThrottleHigh && ARMING_FLAG(ARMED)) {
            beeper(BEEPER_BAT_LOW);
            pTT->startBeepDelay_ms = 1000;
            pTT->timestamp_ms = GetCurrentTime_ms();
            pTT->timestamp2_ms = GetCurrentTime_ms();
            pTT->lastAdjTime_ms = GetCurrentTime_ms();
            pTT->state = TT_WAIT;
            pTT->servoAvgAngle.sum = 0;
            pTT->servoAvgAngle.numOf = 0;
            pTT->tailTuneGyroLimit = 4.5f;
        }
        break;
    case TT_WAIT:
        if (isThrottleHigh && ARMING_FLAG(ARMED)) {
            // Wait for 5 seconds before activating the tuning.
            // This is so that pilot has time to take off if the tail tune mode was activated on ground.
            if (IsDelayElapsed_ms(pTT->timestamp_ms, 5000)) {
                // Longer beep when starting
                beeper(BEEPER_BAT_CRIT_LOW);
                pTT->state = TT_ACTIVE;
                pTT->timestamp_ms = GetCurrentTime_ms();
            } else if (IsDelayElapsed_ms(pTT->timestamp_ms, pTT->startBeepDelay_ms)) {
                // Beep every second until start
                beeper(BEEPER_BAT_LOW);
                pTT->startBeepDelay_ms += 1000;
            }
        } else {
            pTT->state = TT_IDLE;
        }
        break;
    case TT_ACTIVE:
        if (!(isThrottleHigh && isRcAxisWithinDeadband(ROLL) && isRcAxisWithinDeadband(PITCH)
                && isRcAxisWithinDeadband(YAW))) {
            pTT->timestamp_ms = GetCurrentTime_ms(); // sticks are NOT good
        }
        if (fabsf(gyro.gyroADCf[FD_YAW]) > pTT->tailTuneGyroLimit) {
            pTT->timestamp2_ms = GetCurrentTime_ms(); // gyro is NOT stable
        }
        if (IsDelayElapsed_ms(pTT->timestamp_ms, 250)) {
            // RC commands have been within deadbands for 250 ms
            if (IsDelayElapsed_ms(pTT->timestamp2_ms, 250)) {
                // Gyro has also been stable for 250 ms
                if (IsDelayElapsed_ms(pTT->lastAdjTime_ms, 20)) {
                    pTT->lastAdjTime_ms = GetCurrentTime_ms();
                    pTT->servoAvgAngle.sum += triGetCurrentServoAngle();
                    pTT->servoAvgAngle.numOf++;
                    if ((pTT->servoAvgAngle.numOf & 0x1f) == 0x00) {
                        // once every 32 times
                        beeperConfirmationBeeps(1);
                    }
                    if (pTT->servoAvgAngle.numOf >= 500) {
                        beeper(BEEPER_READY_BEEP);
                        pTT->state = TT_WAIT_FOR_DISARM;
                        pTT->timestamp_ms = GetCurrentTime_ms();
                    }
                }
            } else if (IsDelayElapsed_ms(pTT->lastAdjTime_ms, 500)) {
                // Sticks are OK but there has not been any valid samples in 1 s, try to loosen the gyro criteria a little
                pTT->tailTuneGyroLimit += 0.1f;
                pTT->lastAdjTime_ms = GetCurrentTime_ms();
                if (pTT->tailTuneGyroLimit > 10.0f) {
                    // If there are not enough samples by now it is a fail.
                    pTT->state = TT_FAIL;
                }
            }
        }
        break;
    case TT_WAIT_FOR_DISARM:
        if (!ARMING_FLAG(ARMED)) {
            float averageServoAngle = pTT->servoAvgAngle.sum / 10.0f / pTT->servoAvgAngle.numOf;
            if (averageServoAngle > 90.5f && averageServoAngle < 120.f) {
                averageServoAngle -= 90.0f;
                averageServoAngle *= RAD;
                gpTriMixerConfig->tri_tail_motor_thrustfactor = 10.0f * cos_approx(averageServoAngle)
                        / sin_approx(averageServoAngle);

                saveConfigAndNotify();

                pTT->state = TT_DONE;
            } else {
                pTT->state = TT_FAIL;
            }
            pTT->timestamp_ms = GetCurrentTime_ms();
        } else {
            if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000)) {
                beeper(BEEPER_READY_BEEP);
                pTT->timestamp_ms = GetCurrentTime_ms();
            }
        }
        break;
    case TT_DONE:
        if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000)) {
            beeper(BEEPER_READY_BEEP);
            pTT->timestamp_ms = GetCurrentTime_ms();
        }
        break;
    case TT_FAIL:
        if (IsDelayElapsed_ms(pTT->timestamp_ms, 2000)) {
            beeper(BEEPER_ACC_CALIBRATION_FAIL);
            pTT->timestamp_ms = GetCurrentTime_ms();
        }
        break;
    }
}

static void tailTuneModeServoSetup(struct servoSetup_t *pSS, servoParam_t *pServoConf, int16_t *pServoVal)
{
    InitDelayMeasurement_ms();
    // Check mode select
    if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] < -100)) {
        pSS->servoVal = pServoConf->min;
        pSS->pLimitToAdjust = &pServoConf->min;
        beeperConfirmationBeeps(1);
        pSS->state = SS_SETUP;
    } else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] > 100)) {
        pSS->servoVal = pServoConf->middle;
        pSS->pLimitToAdjust = &pServoConf->middle;
        beeperConfirmationBeeps(2);
        pSS->state = SS_SETUP;
    } else if (isRcAxisWithinDeadband(PITCH) && (rcCommand[ROLL] > 100)) {
        pSS->servoVal = pServoConf->max;
        pSS->pLimitToAdjust = &pServoConf->max;
        beeperConfirmationBeeps(3);
        pSS->state = SS_SETUP;
    } else if (isRcAxisWithinDeadband(ROLL) && (rcCommand[PITCH] < -100)) {
        pSS->state = SS_CALIB;
        pSS->cal.state = SS_C_IDLE;
    }
    switch (pSS->state) {
    case SS_IDLE:
        break;
    case SS_SETUP:
        if (!isRcAxisWithinDeadband(YAW)) {
            pSS->servoVal += triGetServoDirection() * -1.0f * (float) rcCommand[YAW] * getdT();
            pSS->servoVal = constrainf(pSS->servoVal, 900.0f, 2100.0f);
            *pSS->pLimitToAdjust = pSS->servoVal;
        }
        break;
    case SS_CALIB:
        // State transition
        if ((pSS->cal.done == true) || (pSS->cal.state == SS_C_IDLE)) {
            if (pSS->cal.state == SS_C_IDLE) {
                pSS->cal.state = SS_C_CALIB_MIN_MID_MAX;
                pSS->cal.subState = SS_C_MIN;
                pSS->servoVal = pServoConf->min;
                pSS->cal.avg.pCalibConfig = &gpTriMixerConfig->tri_servo_min_adc;
            } else if (pSS->cal.state == SS_C_CALIB_SPEED) {
                pSS->state = SS_IDLE;
                pSS->cal.subState = SS_C_IDLE;
                beeper(BEEPER_READY_BEEP);
                // Speed calibration should be done as final step so this saves the min, mid, max and speed values.
                saveConfigAndNotify();
            } else {
                if (pSS->cal.state == SS_C_CALIB_MIN_MID_MAX) {
                    switch (pSS->cal.subState) {
                    case SS_C_MIN:
                        pSS->cal.subState = SS_C_MID;
                        pSS->servoVal = pServoConf->middle;
                        pSS->cal.avg.pCalibConfig = &gpTriMixerConfig->tri_servo_mid_adc;
                        break;
                    case SS_C_MID:
                        if (ABS(gpTriMixerConfig->tri_servo_min_adc - gpTriMixerConfig->tri_servo_mid_adc) < 100) {
                            // Not enough difference between min and mid feedback values.
                            // Most likely the feedback signal is not connected.
                            pSS->state = SS_IDLE;
                            pSS->cal.subState = SS_C_IDLE;
                            beeper(BEEPER_ACC_CALIBRATION_FAIL);
                            // Save configuration even after speed calibration failed.
                            // Speed calibration should be done as final step so this saves the min, mid and max values.
                            saveConfigAndNotify();
                        } else {
                            pSS->cal.subState = SS_C_MAX;
                            pSS->servoVal = pServoConf->max;
                            pSS->cal.avg.pCalibConfig = &gpTriMixerConfig->tri_servo_max_adc;
                        }
                        break;
                    case SS_C_MAX:
                        pSS->cal.state = SS_C_CALIB_SPEED;
                        pSS->cal.subState = SS_C_MIN;
                        pSS->servoVal = pServoConf->min;
                        pSS->cal.waitingServoToStop = true;
                        break;
                    }
                }
            }
            pSS->cal.timestamp_ms = GetCurrentTime_ms();
            pSS->cal.avg.sum = 0;
            pSS->cal.avg.numOf = 0;
            pSS->cal.done = false;
        }
        switch (pSS->cal.state) {
        case SS_C_IDLE:
            break;
        case SS_C_CALIB_MIN_MID_MAX:
            if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 500)) {
                if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 600)) {
                    *pSS->cal.avg.pCalibConfig = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                    pSS->cal.done = true;
                } else {
                    pSS->cal.avg.sum += tailServo.ADC;
                    pSS->cal.avg.numOf++;
                }
            }
            break;
        case SS_C_CALIB_SPEED:
            switch (pSS->cal.subState) {
            case SS_C_MIN:
                // Wait for the servo to reach min position
                if (tailServo.ADC < (gpTriMixerConfig->tri_servo_min_adc + 10)) {
                    if (!pSS->cal.waitingServoToStop) {
                        pSS->cal.avg.sum += GetCurrentDelay_ms(pSS->cal.timestamp_ms);
                        pSS->cal.avg.numOf++;

                        if (pSS->cal.avg.numOf > 5) {
                            const float avgTime = pSS->cal.avg.sum / pSS->cal.avg.numOf;
                            const float avgServoSpeed = (2.0f * tailServo.maxDeflection / 10.0f) / avgTime * 1000.0f;

                            gpTriMixerConfig->tri_tail_servo_speed = avgServoSpeed;
                            tailServo.speed = gpTriMixerConfig->tri_tail_servo_speed;
                            pSS->cal.done = true;
                            pSS->servoVal = pServoConf->middle;
                        }
                        pSS->cal.timestamp_ms = GetCurrentTime_ms();
                        pSS->cal.waitingServoToStop = true;
                    }
                    // Wait for the servo to fully stop before starting speed measuring
                    else if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200)) {
                        pSS->cal.timestamp_ms = GetCurrentTime_ms();
                        pSS->cal.subState = SS_C_MAX;
                        pSS->cal.waitingServoToStop = false;
                        pSS->servoVal = pServoConf->max;
                    }
                }
                break;
            case SS_C_MAX:
                // Wait for the servo to reach max position
                if (tailServo.ADC > (gpTriMixerConfig->tri_servo_max_adc - 10)) {
                    if (!pSS->cal.waitingServoToStop) {
                        pSS->cal.avg.sum += GetCurrentDelay_ms(pSS->cal.timestamp_ms);
                        pSS->cal.avg.numOf++;
                        pSS->cal.timestamp_ms = GetCurrentTime_ms();
                        pSS->cal.waitingServoToStop = true;
                    } else if (IsDelayElapsed_ms(pSS->cal.timestamp_ms, 200)) {
                        pSS->cal.timestamp_ms = GetCurrentTime_ms();
                        pSS->cal.subState = SS_C_MIN;
                        pSS->cal.waitingServoToStop = false;
                        pSS->servoVal = pServoConf->min;
                    }
                }
                break;
            case SS_C_MID:
                // Should not come here
                break;
            }
        }
        break;
    }
    *pServoVal = pSS->servoVal;
}

static void updateServoAngle(float dT)
{
    if (gpTriMixerConfig->tri_servo_feedback == TRI_SERVO_FB_VIRTUAL) {
        tailServo.angle = virtualServoStep(tailServo.angle, tailServo.speed, dT, tailServo.pConf, *tailServo.pOutput);
    } else {
        static pt1Filter_t feedbackFilter;
        // Read new servo feedback signal sample and run it through filter
        const uint16_t ADC = pt1FilterApply4(&feedbackFilter, adcGetChannel(tailServo.ADCChannel), 70, dT);
        tailServo.angle = feedbackServoStep(gpTriMixerConfig, ADC);
        tailServo.ADC = ADC;
    }
}

static AdcChannel getServoFeedbackADCChannel(uint8_t tri_servo_feedback)
{
    AdcChannel channel;
    switch (tri_servo_feedback) {
    case TRI_SERVO_FB_RSSI:
        channel = ADC_RSSI;
        break;
    case TRI_SERVO_FB_CURRENT:
        channel = ADC_CURRENT;
        break;
#ifdef ADC_EXTERNAL
    case TRI_SERVO_FB_EXT1:
        channel = ADC_EXTERNAL;
        break;
#endif
    default:
        channel = ADC_RSSI;
        break;
    }

    return channel;
}

static void predictGyroOnDeceleration(void)
{
    static float previousMotorSpeed = 1000.0f;
    const float tailMotorSpeed = tailMotor.virtualFeedBack;
    // Calculate how much the motor speed changed since last time
    float acceleration = (tailMotorSpeed - previousMotorSpeed);

    previousMotorSpeed = tailMotorSpeed;
    float error = 0;
    if (acceleration < 0.0f)
    {
        // Tests have shown that this is mostly needed when throttle is cut (motor decelerating), so only
        // set the expected gyro error in that case.
        // Set the expected axis error based on tail motor acceleration and configured gain
        error = acceleration * gpTriMixerConfig->tri_motor_acc_yaw_correction * 10.0f;
        error *= sin_approx(DEGREES_TO_RADIANS(triGetCurrentServoAngle() / 10.0f));
    }
    pidSetExpectedGyroError(FD_YAW, error);
}

static float scalePIDBasedOnTailMotorSpeed(float scaledPidOutput, float pidSumLimit)
{
    const float motorFeedbackWoCorrection = constrain(tailMotor.virtualFeedBack - lastMotorCorrection, motorConfig()->minthrottle, motorConfig()->maxthrottle);
    const float fromMin = motorFeedbackWoCorrection - motorConfig()->minthrottle;
    const float percentage = fromMin / (float)throttleRange;
    const float gain = 1.0f - ((1.0f - dynamicYawGainAtMax) * percentage);
    const float pid = constrainf(scaledPidOutput * gain, -pidSumLimit, pidSumLimit);
    return pid;
}

static void tailMotorStep(int16_t setpoint, float dT)
{
    static float current = 1000;
    static pt1Filter_t motorFilter;
    const float dS = dT * tailMotor.acceleration; // Max change of an speed since last check

    if (ABS(current - setpoint) < dS) {
        // At set-point after this moment
        current = setpoint;
    } else if (current < setpoint) {
        current += dS;
    } else {
        current -= dS;
    }
    // Use a PT1 low-pass filter to add "slowness" to the virtual motor feedback.
    // Cut-off to delay:
    // 2  Hz -> 25 ms
    // 5  Hz -> 14 ms
    // 10 Hz -> 9  ms
    tailMotor.virtualFeedBack = pt1FilterApply4(&motorFilter, current, 5, dT);
}

static int8_t triGetServoDirection(void)
{
    const int8_t direction = (int8_t) servoDirection(SERVO_RUDDER, INPUT_STABILIZED_YAW);

    return direction;
}
