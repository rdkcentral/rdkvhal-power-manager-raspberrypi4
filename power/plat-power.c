/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2017 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "plat_power.h"

static PWRMgr_PowerState_t power_state;
static pmStatus_t powerMgrStatus = PWRMGR_NOT_INITIALIZED;

pthread_t worker_thread;
pthread_mutex_t power_state_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief A single shot worker thread to handle the power state changes.
 * RPi does not have proper PowerManager implementation.
 * Some references:
 * https://learn.pi-supply.com/make/how-to-save-power-on-your-raspberry-pi/
 * https://forums.raspberrypi.com/viewtopic.php?t=257144
 * https://blues.com/blog/tips-tricks-optimizing-raspberry-pi-power/
 */
static void *powerMgrWorkerThread(void *arg) {
    PWRMgr_PowerState_t received_state;

    pthread_mutex_lock(&power_state_mutex);
    received_state = *(PWRMgr_PowerState_t*)arg;
    pthread_mutex_unlock(&power_state_mutex);

    printf("Power state: %d\n", received_state);
    switch (received_state) {
        case PWRMGR_POWERSTATE_OFF:
            printf("Powering off\n");
            system("poweroff");
            break;
        case PWRMGR_POWERSTATE_STANDBY:
            printf("Powering to standby\n");
            break;
        case PWRMGR_POWERSTATE_ON:
            printf("Powering on\n");
            break;
        case PWRMGR_POWERSTATE_STANDBY_LIGHT_SLEEP:
            printf("Powering to standby light sleep\n");
            break;
        case PWRMGR_POWERSTATE_STANDBY_DEEP_SLEEP:
            printf("Powering to standby deep sleep\n");
            break;
        default:
            printf("Invalid power state\n");
            break;
    }
    return NULL;
}

/**
 * @brief Initialize the underlying Power Management module.
 *
 * This function must initialize all aspects of the CPE's Power Management module.
 *
 * @param None.
 * @return    Return Code.
 * @retval    0 if successful.
 */
pmStatus_t PLAT_INIT(void)
{
    if (PWRMGR_NOT_INITIALIZED == powerMgrStatus) {
        pthread_mutex_lock(&power_state_mutex);
        power_state = PWRMGR_POWERSTATE_ON;
        pthread_mutex_unlock(&power_state_mutex);
        /* TODO: Act as per new state change requested. */
        if (pthread_create(&worker_thread, NULL, powerMgrWorkerThread, &power_state) != 0) {
            perror("Failed to create worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        if (pthread_detach(worker_thread) != 0) {
            perror("Failed to detach worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        powerMgrStatus = PWRMGR_ALREADY_INITIALIZED;
        return PWRMGR_SUCCESS;
    }

    return PWRMGR_ALREADY_INITIALIZED;
}

/**
 * @brief Set the CPE Power State.
 *
 * This function sets the CPE's current power state to the specified state.
 *
 * @param [in]  newState    The power state to which to set the CPE.
 * @return    Return Code.
 * @retval    0 if successful.
 */
pmStatus_t PLAT_API_SetPowerState( PWRMgr_PowerState_t newState )
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    // Verify that newState is one among PWRMgr_PowerState_t
    if (newState >= PWRMGR_POWERSTATE_OFF && newState < PWRMGR_POWERSTATE_MAX) {
        pthread_mutex_lock(&power_state_mutex);
        power_state = newState;
        pthread_mutex_unlock(&power_state_mutex);
        /* TODO: Act as per new state change requested. */
        if (pthread_create(&worker_thread, NULL, powerMgrWorkerThread, &power_state) != 0) {
            perror("Failed to create worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        if (pthread_detach(worker_thread) != 0) {
            perror("Failed to detach worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        return PWRMGR_SUCCESS;
    }

    return PWRMGR_INVALID_ARGUMENT;
}

/**
 * @brief Get the CPE Power State.
 *
 * This function returns the current power state of the CPE.
 *
 * @param [in]  curState    The address of a location to hold the current power state of
 *                          the CPE on return.
 * @return    Return Code.
 * @retval    0 if successful.
 */
pmStatus_t PLAT_API_GetPowerState( PWRMgr_PowerState_t *curState )
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }

    if (NULL == curState) {
        return PWRMGR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&power_state_mutex);
    *curState = power_state;
    pthread_mutex_unlock(&power_state_mutex);

    return PWRMGR_SUCCESS;
}

/**
 * @brief Enables or disables the Wakeup source type
 *
 * @param [in] srcType  - Wake up source type
 * @param [in] enable   - Enable or disable Wake up source
 *                        True for enabled, false for disabled
 *
 * @return    pmStatus_t                        - Status
 * @retval    PWRMGR_SUCCESS                    - Success
 * @retval    PWRMGR_NOT_INITIALIZED            - Module is not initialised
 * @retval    PWRMGR_OPERATION_NOT_SUPPORTED    - Wake up source type not supported
 * @retval    PWRMGR_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 * @retval    PWRMGR_SET_FAILURE                - Failed to  power state
 *
 * @pre PLAT_INIT() must be called before calling this API
 *
 * @warning This API is Not thread safe
 *
 * @see PLAT_API_GetWakeupSrc(), PWRMGR_WakeupSrcType_t
 */
pmStatus_t PLAT_API_SetWakeupSrc(PWRMGR_WakeupSrcType_t srcType, bool enable) {
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    if (!(srcType >= PWRMGR_WAKEUPSRC_VOICE && srcType < PWRMGR_WAKEUPSRC_MAX)) {
        return PWRMGR_INVALID_ARGUMENT;
    }
    /* FIXME: RPi don't have WakeUp Source configurations at the moment. */
    return PWRMGR_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Checks if the wake up source is enabled or disabled for the device
 *
 * @param [in] srcType  - Wake up source type
 * @param [out] enable  - Variable to store if wake up source type is enabled or disabled
 *                        True for enabled, false for disabled
 *
 * @return    pmStatus_t                        - Status
 * @retval    PWRMGR_SUCCESS                    - Success
 * @retval    PWRMGR_NOT_INITIALIZED            - Module is not initialised
 * @retval    PWRMGR_OPERATION_NOT_SUPPORTED    - Wake up source type not supported
 * @retval    PWRMGR_INVALID_ARGUMENT           - Parameter passed to this function is invalid
 * @retval    PWRMGR_GET_FAILURE                - Failed to get
 *
 * @pre PLAT_INIT() must be called before calling this API
 *
 * @warning This API is Not thread safe
 *
 * @see PWRMGR_WakeupSrcType_t, PLAT_API_SetWakeupSrc()
 */
pmStatus_t PLAT_API_GetWakeupSrc(PWRMGR_WakeupSrcType_t srcType, bool  *enable) {
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    if (!(srcType >= PWRMGR_WAKEUPSRC_VOICE && srcType < PWRMGR_WAKEUPSRC_MAX) || NULL == enable) {
        return PWRMGR_INVALID_ARGUMENT;
    }
    /* FIXME: RPi don't have WakeUp Source configurations at the moment. */
    return PWRMGR_OPERATION_NOT_SUPPORTED;
}

#ifdef ENABLE_THERMAL_PROTECTION

static float g_fTempThresholdHigh = 60.0f;
static float g_fTempThresholdCritical = 75.0f;

/**
 * @brief Get the  current temperature of the core.
 *
 * @param[out] state            The current state of the core temperature
 * @param[out] curTemperature   Raw temperature value of the core
 *                              in degrees Celsius
 * @param[out] wifiTemperature  Raw temperature value of the wifi chip
 *                              in degrees Celsius
 *
 * @return Returns the status of the operation.
 * @retval 0 if successful, appropiate error code otherwise.
*/
int PLAT_API_GetTemperature(IARM_Bus_PWRMgr_ThermalState_t *curState, float *curTemperature, float *wifiTemperature)
{
    if ( curState == NULL || curTemperature == NULL || wifiTemperature == NULL )
        return IARM_RESULT_INVALID_PARAM;

    int value = 0;
    float temp = 0.0f;
    IARM_Bus_PWRMgr_ThermalState_t state = IARM_BUS_PWRMGR_TEMPERATURE_NORMAL;

    FILE* fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if( fp != NULL )
    {
        fscanf (fp, "%d", &value);
        fclose(fp);
    }

    temp = value / 1000;
    *curTemperature = temp;

    if( temp >= g_fTempThresholdHigh )
        state = IARM_BUS_PWRMGR_TEMPERATURE_HIGH;
    if( temp >= g_fTempThresholdCritical )
        state = IARM_BUS_PWRMGR_TEMPERATURE_CRITICAL;

    *curState = state;
    return IARM_RESULT_SUCCESS;
}

/**
 * @brief Set temperature thresholds which will determine the state returned from a call to mfrGetTemperature.
 *
 * @param[in] tempHigh       Temperature threshold at which mfrTEMPERATURE_HIGH
 *                           state will be reported.
 * @param[in] tempCritical   Temperature threshold at which mfrTEMPERATURE_CRITICAL
 *                           state will be reported.
 *
 * @return  Returns the status of the operation.
 * @retval  0 if successful, appropiate error code otherwise.
 */
int PLAT_API_SetTempThresholds(float tempHigh, float tempCritical)
{
    g_fTempThresholdHigh     = tempHigh;
    g_fTempThresholdCritical = tempCritical;

    return IARM_RESULT_SUCCESS;
}

/**
 * @brief Get temperature thresholds which will determine the state returned from a call to mfrGetTemperature.
 *
 * @param[out] tempHigh      Temperature threshold at which mfrTEMPERATURE_HIGH
 *                           state will be reported.
 * @param[out] tempCritical  Temperature threshold at which mfrTEMPERATURE_CRITICAL
 *                           state will be reported.
 *
 * @return Returns the status of the operation.
 * @retval 0 if successful, appropiate error code otherwise.
 */
int PLAT_API_GetTempThresholds(float *tempHigh, float *tempCritical)
{
    if( tempHigh == NULL || tempCritical == NULL )
        return IARM_RESULT_INVALID_PARAM;

    *tempHigh     = g_fTempThresholdHigh;
    *tempCritical = g_fTempThresholdCritical;

    return IARM_RESULT_SUCCESS;
}

/**
 * @brief Get clock speeds for this device for the given states
 *
 * @param [out] cpu_rate_Normal  The clock rate to be used when in the 'normal' state
 * @param [out] cpu_rate_Scaled  The clock rate to be used when in the 'scaled' state
 * @param [out] cpu_rate_Minimal The clock rate to be used when in the 'minimal' state
 *
 * @return 1 if operation is attempted 0 otherwise
 */
int PLAT_API_DetemineClockSpeeds(uint32_t *cpu_rate_Normal, uint32_t *cpu_rate_Scaled, uint32_t *cpu_rate_Minimal)
{
    return IARM_RESULT_SUCCESS;
}

/**
 * @brief This API sets the clock speed of the CPU.
 * @param [in] speed  One of the predefined parameters to set the clock speed.
 *
 * @return Returns the status of the operation
 * @retval returns 1, if operation is attempted and 0 otherwise
 */
int PLAT_API_SetClockSpeed(uint32_t speed)
{
    return IARM_RESULT_SUCCESS;
}

/**
 * @brief This API returns the clock speed of the CPU
 *
 * @param [out] speed One of the predefined parameters
 * @return Returns the current clock speed.
 */
int PLAT_API_GetClockSpeed(uint32_t *speed)
{
    return IARM_RESULT_SUCCESS;
}

#endif //ENABLE_THERMAL_PROTECTION

/**
 * @brief Close the IR device module.
 *
 * This function must terminate the CPE Power Management module. It must reset any data
 * structures used within Power Management module and release any Power Management
 * specific handles and resources.
 *
 * @param None.
 * @return None.
 */
pmStatus_t PLAT_TERM( void )
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    powerMgrStatus = PWRMGR_NOT_INITIALIZED;
    return PWRMGR_SUCCESS;
}

pmStatus_t PLAT_Reset( PWRMgr_PowerState_t newState )
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    // Verify that newState is one among PWRMgr_PowerState_t
    if (newState >= PWRMGR_POWERSTATE_OFF && newState < PWRMGR_POWERSTATE_MAX) {
        pthread_mutex_lock(&power_state_mutex);
        power_state = newState;
        pthread_mutex_unlock(&power_state_mutex);
        /* TODO: Act as per new state change requested. */
        if (pthread_create(&worker_thread, NULL, powerMgrWorkerThread, &power_state) != 0) {
            perror("Failed to create worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        if (pthread_detach(worker_thread) != 0) {
            perror("Failed to detach worker thread");
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }
        return PWRMGR_SUCCESS;
    }
    return PWRMGR_INVALID_ARGUMENT;
}

void PLAT_WHReset()
{
    return;
}

void PLAT_FactoryReset()
{
    return;
}

