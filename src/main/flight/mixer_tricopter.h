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

#pragma once

#include "servos.h"

#define TAIL_THRUST_FACTOR_MIN  (10)
#define TAIL_THRUST_FACTOR_MAX  (400)

#define TAIL_THRUST_FACTOR_MIN_FLOAT (TAIL_THRUST_FACTOR_MIN / 10.0f)
#define TAIL_THRUST_FACTOR_MAX_FLOAT (TAIL_THRUST_FACTOR_MAX / 10.0f)

#define TRI_MOTOR_ACC_CORRECTION_MAX  (200)

/** @brief Servo feedback sources. */
typedef enum {
    TRI_SERVO_FB_VIRTUAL = 0,  // Virtual servo, no physical feedback signal from servo
    TRI_SERVO_FB_RSSI,         // Feedback signal from RSSI ADC
    TRI_SERVO_FB_CURRENT,      // Feedback signal from CURRENT ADC
    TRI_SERVO_FB_EXT1,         // Feedback signal from EXT1 ADC
} triServoFeedbackSource_e;

/** @brief Initialize tricopter specific mixer functionality.
 *
 *  @param pTailServoConfig Pointer to tail servo configuration
 *  when in tricopter mixer mode.
 *  @param pTailServo Pointer to tail servo output value.
 *  @return Void.
 */
void triInitMixer(servoParam_t *pTailServoConfig, int16_t *pTailServo);

/** @brief Get current tail servo angle.
 *
 *  @return Servo angle in decidegrees.
 */
uint16_t triGetCurrentServoAngle(void);

/** @brief Perform tricopter mixer actions.
 *
 *  @param PIDoutput output from PID controller, in scale of [-1000, 1000].
 *  @return Void.
 */
void triServoMixer(int16_t PIDoutput);

/** @brief Get amount of motor correction that must be applied
 * for given motor.
 *
 * Needed correction amount is calculated based on current servo
 * position to maintain pitch axis attitude.
 *
 *  @return Amount of motor correction that must be added to
 *  motor output.
 */
int16_t triGetMotorCorrection(uint8_t motorIndex);

/** @brief Should tail servo be active when unarmed.
 *
 *  @return true is should, otherwise false.
 */
_Bool triIsEnabledServoUnarmed(void);

/** @brief Is tricopter mixer in use.
 *
 *  @return true if is, otherwise false.
 */
_Bool triMixerInUse(void);

typedef struct triMixerConfig_s{
    uint8_t tri_unarmed_servo;              // send tail servo correction pulses even when unarmed
    uint8_t tri_servo_feedback;
    int16_t tri_tail_motor_thrustfactor;
    int16_t tri_tail_servo_speed;
    uint16_t tri_servo_min_adc;
    uint16_t tri_servo_mid_adc;
    uint16_t tri_servo_max_adc;
    uint16_t tri_motor_acc_yaw_correction;
    uint16_t dummy;
    uint8_t tri_motor_acceleration;
    uint16_t tri_dynamic_yaw_minthrottle;
    uint16_t tri_dynamic_yaw_maxthrottle;
    uint16_t tri_servo_angle_at_max;
} triMixerConfig_t;

PG_DECLARE(triMixerConfig_t, triMixerConfig);

#ifdef MIXER_TRICOPTER_INTERNALS

typedef enum {
    TT_IDLE = 0,
    TT_WAIT,
    TT_ACTIVE,
    TT_WAIT_FOR_DISARM,
    TT_DONE,
    TT_FAIL,
} tailTuneState_e;

typedef enum {
    SS_IDLE = 0,
    SS_SETUP,
    SS_CALIB,
} servoSetupState_e;

typedef enum {
    SS_C_IDLE = 0,
    SS_C_CALIB_MIN_MID_MAX,
    SS_C_CALIB_SPEED,
} servoSetupCalibState_e;

typedef enum {
    SS_C_MIN = 0,
    SS_C_MID,
    SS_C_MAX,
} servoSetupCalibSubState_e;

typedef enum {
    TT_MODE_NONE = 0,
    TT_MODE_THRUST_TORQUE,
    TT_MODE_SERVO_SETUP,
} tailtuneMode_e;

typedef struct servoAvgAngle_s {
    uint32_t sum;
    uint16_t numOf;
} servoAvgAngle_t;

typedef struct thrustTorque_s {
    tailTuneState_e state;
    uint32_t startBeepDelay_ms;
    uint32_t timestamp_ms;
    uint32_t timestamp2_ms;
    uint32_t lastAdjTime_ms;
    servoAvgAngle_t servoAvgAngle;
    float tailTuneGyroLimit;
} thrustTorque_t;

typedef struct tailServo_s {
    int32_t maxYawForce;
    float thrustFactor;
    int16_t maxAngle;
    int16_t speed;
    uint16_t angle;
    uint16_t ADC;
} tailServo_t;

typedef struct tailMotor_s {
    int16_t pitchZeroAngle;
    int16_t accelerationDelay_ms;
    int16_t decelerationDelay_ms;
    int16_t accelerationDelay_angle;
    int16_t decelerationDelay_angle;
    float virtualFeedBack;
} tailMotor_t;

typedef struct tailTune_s {
    tailtuneMode_e mode;
    thrustTorque_t tt;
    struct servoSetup_t {
        servoSetupState_e state;
        float servoVal;
        int16_t *pLimitToAdjust;
        struct servoCalib_t {
            _Bool done;
            _Bool waitingServoToStop;
            servoSetupCalibState_e state;
            servoSetupCalibSubState_e subState;
            uint32_t timestamp_ms;
            struct average_t {
                uint16_t *pCalibConfig;
                uint32_t sum;
                uint16_t numOf;
            } avg;
        } cal;
    } ss;
} tailTune_t;

#endif /* MIXER_TRICOPTER_INTERNALS */
