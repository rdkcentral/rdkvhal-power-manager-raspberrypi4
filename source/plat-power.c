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
#include <semaphore.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

#include "plat_power.h"

#define PWRHALVERSION "1.2.0"

static PWRMgr_PowerState_t power_state;
static pmStatus_t powerMgrStatus = PWRMGR_NOT_INITIALIZED;

pthread_t worker_thread;
pthread_mutex_t power_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t power_state_cond = PTHREAD_COND_INITIALIZER;
sem_t power_state_semaphore;
int thread_running = 1;

// RPi4 Specific tunings. These are the options for the CPU frequency scaling governor:
// conservative, ondemand, userspace, powersave, performance, schedutil
/***
 ** Some references:
 * https://learn.pi-supply.com/make/how-to-save-power-on-your-raspberry-pi/
 * https://forums.raspberrypi.com/viewtopic.php?t=257144
 * https://blues.com/blog/tips-tricks-optimizing-raspberry-pi-power/
conservative:
    Gradually increases and decreases the CPU frequency based on system load.
    Aims to save power while providing adequate performance.
    Suitable for systems where power efficiency is important.
ondemand:
    Quickly increases the CPU frequency to the maximum when the system load increases.
    Reduces the frequency when the load decreases.
    Provides a balance between performance and power saving.
userspace:
    Allows user-space programs to set the CPU frequency.
    Provides flexibility for custom frequency management policies.
powersave:
    Sets the CPU frequency to the minimum available.
    Maximizes power saving at the cost of performance.
    Suitable for scenarios where power consumption is critical.
performance:
    Sets the CPU frequency to the maximum available.
    Maximizes performance at the cost of higher power consumption.
    Suitable for performance-critical applications.
schedutil:
    Integrates with the Linux kernel's scheduler to dynamically adjust the CPU frequency.
    Aims to provide a balance between performance and power saving based on real-time system load.
    Generally considered more efficient and responsive compared to other governors.
*/
#define CPU_FREQ_SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

/**
 * @brief Get the CPU frequency scaling governor.
 * @param governor The buffer to store the CPU frequency scaling governor.
 * @param size The size of the buffer.
 * @return true if successful, false otherwise.
*/
bool getCPUFreqScalingGovernor(char *governor, size_t size)
{
    char buffer[32] = {0};
    if (NULL == governor) {
        return false;
    }
    FILE *fp = fopen(CPU_FREQ_SCALING_GOVERNOR_PATH, "r");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling governor file");
        return false;
    }
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        perror("Failed to read CPU frequency scaling governor file");
        fclose(fp);
        return false;
    }
    fclose(fp);
    printf("CPU frequency scaling governor: '%s'\n", buffer);
    snprintf(governor, size, "%s", buffer);
    return true;
}

/**
 * @brief Set the CPU frequency scaling governor.
 * @param governor The CPU frequency scaling governor to set:
 *      "conservative", "ondemand", "userspace", "powersave", "performance", "schedutil".
 * @return true if successful, false otherwise.
*/
bool setCPUFreqScalingGovernor(const char *governor)
{
    if (NULL == governor) {
        return false;
    }
    if ((strcmp(governor, "conservative") != 0) &&
        (strcmp(governor, "ondemand") != 0) &&
        (strcmp(governor, "userspace") != 0) &&
        (strcmp(governor, "powersave") != 0) &&
        (strcmp(governor, "performance") != 0) &&
        (strcmp(governor, "schedutil") != 0)) {
        printf("Invalid CPU frequency scaling governor: '%s'\n", governor);
        return false;
    }
    FILE *fp = fopen(CPU_FREQ_SCALING_GOVERNOR_PATH, "w");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling governor file");
        return false;
    }
    if (fputs(governor, fp) == EOF) {
        perror("Failed to write CPU frequency scaling governor file");
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

/**
 * @brief Convert the RDK power state Enum to string.
 * @param state The RDK power state enum.
 * @return The string representation of the power state.
*/
char * rdkPowerStateToString(PWRMgr_PowerState_t state)
{
    switch (state) {
        case PWRMGR_POWERSTATE_OFF:
            return "PWRMGR_POWERSTATE_OFF";
        case PWRMGR_POWERSTATE_STANDBY:
            return "PWRMGR_POWERSTATE_STANDBY";
        case PWRMGR_POWERSTATE_ON:
            return "PWRMGR_POWERSTATE_ON";
        case PWRMGR_POWERSTATE_STANDBY_LIGHT_SLEEP:
            return "PWRMGR_POWERSTATE_STANDBY_LIGHT_SLEEP";
        case PWRMGR_POWERSTATE_STANDBY_DEEP_SLEEP:
            return "PWRMGR_POWERSTATE_STANDBY_DEEP_SLEEP";
        default:
            return "Invalid power state";
    }
}

/**
 * @brief A single shot worker thread to handle the power state changes.
 * RPi does not have proper PowerManager implementation.
 * Some references:
 * https://learn.pi-supply.com/make/how-to-save-power-on-your-raspberry-pi/
 * https://forums.raspberrypi.com/viewtopic.php?t=257144
 * https://blues.com/blog/tips-tricks-optimizing-raspberry-pi-power/
 */
static void *powerMgrWorkerThread(void *arg)
{
    while (1) {
        if (sem_wait(&power_state_semaphore) == -1) {
            perror("powerMgrWorkerThread: Failed to wait on semaphore");
            break;
        }

        if (pthread_mutex_lock(&power_state_mutex) != 0) {
            perror("powerMgrWorkerThread: Failed to lock mutex");
            continue;
        }

        if (!thread_running) {
            if (pthread_mutex_unlock(&power_state_mutex) != 0) {
                perror("powerMgrWorkerThread: Failed to unlock mutex");
            }
            break;
        }

        PWRMgr_PowerState_t received_state = power_state;
        if (pthread_mutex_unlock(&power_state_mutex) != 0) {
            perror("powerMgrWorkerThread: Failed to unlock mutex");
        }

        printf("powerMgrWorkerThread: Power state change to '[%u] %s'.\n",
                received_state, rdkPowerStateToString(received_state));

        switch (received_state) {
            case PWRMGR_POWERSTATE_OFF:
                printf("powerMgrWorkerThread: Powering off\n");
                sync();
                if (reboot(RB_POWER_OFF) != 0) {
                    perror("powerMgrWorkerThread: Failed to power off");
                }
                break;
            case PWRMGR_POWERSTATE_STANDBY:
                printf("powerMgrWorkerThread: Powering to standby\n");
                if (!setCPUFreqScalingGovernor("powersave")) {
                    perror("powerMgrWorkerThread: Failed to set CPU frequency scaling governor to 'powersave'");
                }
                break;
            case PWRMGR_POWERSTATE_ON:
                printf("powerMgrWorkerThread: Powering on\n");
                if (!setCPUFreqScalingGovernor("performance")) {
                    perror("powerMgrWorkerThread: Failed to set CPU frequency scaling governor to 'performance'");
                }
                break;
            case PWRMGR_POWERSTATE_STANDBY_LIGHT_SLEEP:
                printf("powerMgrWorkerThread: Powering to standby light sleep\n");
                if (!setCPUFreqScalingGovernor("ondemand")) {
                    perror("powerMgrWorkerThread: Failed to set CPU frequency scaling governor to 'ondemand'");
                }
                break;
            case PWRMGR_POWERSTATE_STANDBY_DEEP_SLEEP:
                printf("powerMgrWorkerThread: Powering to standby deep sleep\n");
                if (!setCPUFreqScalingGovernor("conservative")) {
                    perror("powerMgrWorkerThread: Failed to set CPU frequency scaling governor to 'conservative'");
                }
                break;
            default:
                printf("powerMgrWorkerThread: Invalid power state\n");
                break;
        }
        sync();
    }
    return NULL;
}

/**
 * @brief Initializes the underlying Power Management module
 * This function must initialize all aspects of the CPE's Power Management module.
 * @return    pmStatus_t                    - Status
 * @retval    PWRMGR_SUCCESS                - Success
 * @retval    PWRMGR_ALREADY_INITIALIZED    - Function is already initialized
 * @retval    PWRMGR_INIT_FAILURE           - Function has failed to properly initialize
 */
pmStatus_t PLAT_INIT(void)
{
    fprintf(stderr, "PLAT_INIT: PowerMgr HAL version: %s\n", PWRHALVERSION);

    if (access(CPU_FREQ_SCALING_GOVERNOR_PATH, F_OK | R_OK | W_OK) != 0) {
        perror("PLAT_INIT: Failed to access CPU frequency scaling governor file");
        return PWRMGR_INIT_FAILURE;
    }
    if (PWRMGR_NOT_INITIALIZED == powerMgrStatus) {
        if (pthread_mutex_lock(&power_state_mutex) != 0) {
            perror("PLAT_INIT: Failed to lock mutex");
            return PWRMGR_INIT_FAILURE;
        }
        power_state = PWRMGR_POWERSTATE_ON;
        if (pthread_mutex_unlock(&power_state_mutex) != 0) {
            perror("PLAT_INIT: Failed to unlock mutex");
            return PWRMGR_INIT_FAILURE;
        }

        if (sem_init(&power_state_semaphore, 0, 0) != 0) {
            perror("PLAT_INIT: Failed to initialize semaphore");
            return PWRMGR_INIT_FAILURE;
        }
        thread_running = 1;

        if (pthread_create(&worker_thread, NULL, powerMgrWorkerThread, NULL) != 0) {
            perror("PLAT_INIT: Failed to create worker thread");
            sem_destroy(&power_state_semaphore); // Clean up semaphore
            return PWRMGR_OPERATION_NOT_SUPPORTED;
        }

        powerMgrStatus = PWRMGR_ALREADY_INITIALIZED;
        printf("PLAT_INIT: HAL init success.\n");
        return PWRMGR_SUCCESS;
    }

    return PWRMGR_ALREADY_INITIALIZED;
}

/**
 * @brief Sets the CPE Power State
 * This fumction is just required to hold the value of the current power state status.
 * @param[in]  newState - The new power state value
 * @return    pmStatus_t                - Status
 * @retval    PWRMGR_SUCCESS            - Success
 * @retval    PWRMGR_NOT_INITIALIZED    - Module is not initialised
 * @retval    PWRMGR_INVALID_ARGUMENT   - Parameter passed to this function is invalid
 * @retval    PWRMGR_SET_FAILURE        - Failed to update
 * @pre PLAT_INIT() must be called before calling this API
 * @warning This API is Not thread safe
 * @see PLAT_API_GetPowerState(), PWRMgr_PowerState_t
 */
pmStatus_t PLAT_API_SetPowerState(PWRMgr_PowerState_t newState)
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }
    if (newState >= PWRMGR_POWERSTATE_OFF && newState < PWRMGR_POWERSTATE_MAX) {
        if (pthread_mutex_lock(&power_state_mutex) != 0) {
            perror("PLAT_API_SetPowerState: Failed to lock mutex");
            return PWRMGR_SET_FAILURE;
        }
        power_state = newState;
        if (pthread_mutex_unlock(&power_state_mutex) != 0) {
            perror("PLAT_API_SetPowerState: Failed to unlock mutex");
            return PWRMGR_SET_FAILURE;
        }

        if (sem_post(&power_state_semaphore) != 0) {
            perror("PLAT_API_SetPowerState: Failed to post semaphore");
            return PWRMGR_SET_FAILURE;
        }
        return PWRMGR_SUCCESS;
    }

    return PWRMGR_INVALID_ARGUMENT;
}

/**
 * @brief Gets the CPE Power State
 * @param[out] curState  - The current power state of the CPE
 * @return    pmStatus_t                - Status
 * @retval    PWRMGR_SUCCESS            - Success
 * @retval    PWRMGR_NOT_INITIALIZED    - Module is not initialised
 * @retval    PWRMGR_INVALID_ARGUMENT   - Parameter passed to this function is invalid
 * @retval    PWRMGR_GET_FAILURE        - Failed to get
 * @pre PLAT_INIT() must be called before calling this API
 * @warning This API is Not thread safe
 * @see PLAT_API_SetPowerState(), PWRMgr_PowerState_t
 */
pmStatus_t PLAT_API_GetPowerState(PWRMgr_PowerState_t *curState)
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }

    if (NULL == curState) {
        return PWRMGR_INVALID_ARGUMENT;
    }

    if (pthread_mutex_lock(&power_state_mutex) != 0) {
        perror("PLAT_API_GetPowerState: Failed to lock mutex");
        return PWRMGR_GET_FAILURE;
    }

    *curState = power_state;

    if (pthread_mutex_unlock(&power_state_mutex) != 0) {
        perror("PLAT_API_GetPowerState: Failed to unlock mutex");
        return PWRMGR_GET_FAILURE;
    }

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
 * @warning This API is Not thread safe
 * @see PLAT_API_GetWakeupSrc(), PWRMGR_WakeupSrcType_t
 */
pmStatus_t PLAT_API_SetWakeupSrc(PWRMGR_WakeupSrcType_t srcType, bool enable)
{
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
pmStatus_t PLAT_API_GetWakeupSrc(PWRMGR_WakeupSrcType_t srcType, bool  *enable)
{
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

#define CPU_FREQ_SCALING_CUR_FREQ_PATH  "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define CPU_FREQ_SCALING_SETSPEED_PATH  "/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed"
#define CPU_FREQ_SCALING_CUR_FREQ_PATH  "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#define THERMAL_ZONE_TEMPERATURE_PATH   "/sys/class/thermal/thermal_zone0/temp"

/* Warning: not tuned; before enabling; finetune these */
#define NORMAL_CLOCK_SPEED  1000 // value for normal state
#define SCALED_CLOCK_SPEED  900 // value for scaled state
#define MINIMAL_CLOCK_SPEED 600  // value for minimal state

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
int PLAT_API_GetTemperature(mfrTemperatureState_t *curState, float *curTemperature, float *wifiTemperature)
{
    if ( curState == NULL || curTemperature == NULL || wifiTemperature == NULL ) {
        return mfrERR_INVALID_PARAM;
    }

    int value = 0;
    float temp = 0.0f;
    IARM_Bus_PWRMgr_ThermalState_t state = mfrTEMPERATURE_NORMAL;

    if (access(THERMAL_ZONE_TEMPERATURE_PATH, F_OK | R_OK) != 0) {
        perror("PLAT_API_GetTemperature: Failed to access thermal zone0 temp file");
        return mfrERR_TEMP_READ_FAILED;
    }

    FILE* fp = fopen(THERMAL_ZONE_TEMPERATURE_PATH, "r");
    if (fp == NULL) {
        perror("PLAT_API_GetTemperature: Failed to open thermal zone0 temp file");
        return mfrERR_TEMP_READ_FAILED;
    }

    if (fscanf(fp, "%d", &value) != 1) {
        perror("PLAT_API_GetTemperature: Failed to read thermal zone0 temp file");
        fclose(fp);
        return mfrERR_TEMP_READ_FAILED;
    }

    temp = value / 1000;
    *curTemperature = temp;

    if( temp >= g_fTempThresholdHigh )
        state = mfrTEMPERATURE_HIGH;
    if( temp >= g_fTempThresholdCritical )
        state = mfrTEMPERATURE_CRITICAL;

    *curState = state;
    return mfrERR_NONE;
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
    if (tempHigh < 0 || tempCritical < 0) {
        printf("PLAT_API_SetTempThresholds: Temperature thresholds must be non-negative\n");
        return mfrERR_INVALID_PARAM;
    }

    if (tempCritical < tempHigh) {
        printf("PLAT_API_SetTempThresholds: Critical temperature threshold must be greater than or equal to high temperature threshold\n");
        return mfrERR_INVALID_PARAM;
    }

    g_fTempThresholdHigh = tempHigh;
    g_fTempThresholdCritical = tempCritical;

    return mfrERR_NONE;
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
    if (tempHigh == NULL || tempCritical == NULL) {
        return mfrERR_INVALID_PARAM;
    }

    *tempHigh = g_fTempThresholdHigh;
    *tempCritical = g_fTempThresholdCritical;

    return mfrERR_NONE;
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
    if (cpu_rate_Normal == NULL || cpu_rate_Scaled == NULL || cpu_rate_Minimal == NULL) {
        return mfrERR_INVALID_PARAM;
    }
#if 0 /* Enable when proper HAL Spec is available. */
    // Return predefined clock speeds
    *cpu_rate_Normal = NORMAL_CLOCK_SPEED;
    *cpu_rate_Scaled = SCALED_CLOCK_SPEED;
    *cpu_rate_Minimal = MINIMAL_CLOCK_SPEED;

    if (access(CPU_FREQ_SCALING_CUR_FREQ_PATH, F_OK | R_OK) != 0) {
        perror("Failed to access CPU frequency scaling current frequency file");
        return mfrERR_FLASH_READ_FAILED;
    }

    FILE *fp = fopen(CPU_FREQ_SCALING_CUR_FREQ_PATH, "r");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling current frequency file");
        return mfrERR_FLASH_READ_FAILED;
    }

    int value = 0;
    if (fscanf(fp, "%d", &value) != 1) {
        perror("Failed to read CPU frequency scaling current frequency file");
        fclose(fp);
        return mfrERR_FLASH_READ_FAILED;
    }
    fclose(fp);

    // Value read is in kHz, convert it to MHz
    uint32_t current_speed = value / 1000;
    printf("Current CPU clock speed: %u MHz\n", current_speed);
    return mfrERR_NONE;
#else
    printf("PLAT_API_DetemineClockSpeeds: Not implemented\n");
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif
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
#if 0 /* Enable when proper HAL Spec is available. */
    uint32_t speed_khz = speed * 1000;
    if (access(CPU_FREQ_SCALING_SETSPEED_PATH, F_OK | W_OK) != 0) {
        perror("Failed to access CPU frequency scaling set speed file");
        return mfrERR_FLASH_READ_FAILED;
    }
    // Check if the scaling_setspeed file is supported
    FILE *fp = fopen(CPU_FREQ_SCALING_SETSPEED_PATH, "r");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling set speed file for reading");
        return mfrERR_FLASH_READ_FAILED;
    }

    char buffer[32] = {0};
    if (fgets(buffer, sizeof(buffer), fp) == NULL) {
        perror("Failed to read CPU frequency scaling set speed file");
        fclose(fp);
        return mfrERR_FLASH_READ_FAILED;
    }
    fclose(fp);

    if (strncmp(buffer, "<unsupported>", 13) == 0) {
        fprintf(stderr, "CPU frequency scaling set speed is unsupported\n");
        return mfrERR_OPERATION_NOT_SUPPORTED;
    }

    fp = fopen(CPU_FREQ_SCALING_SETSPEED_PATH, "w");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling set speed file for writing");
        return mfrERR_WRITE_FLASH_FAILED;
    }

    if (fprintf(fp, "%u", speed_khz) < 0) {
        perror("Failed to write CPU frequency scaling set speed file");
        fclose(fp);
        return mfrERR_WRITE_FLASH_FAILED;
    }

    if (fclose(fp) != 0) {
        perror("Failed to close CPU frequency scaling set speed file");
        return mfrERR_WRITE_FLASH_FAILED;
    }
    return mfrERR_NONE;
#else
    printf("PLAT_API_SetClockSpeed: Not implemented\n");
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif
}

/**
 * @brief This API returns the clock speed of the CPU
 *
 * @param [out] speed One of the predefined parameters
 * @return Returns the current clock speed.
 */
int PLAT_API_GetClockSpeed(uint32_t *speed)
{
    if (speed == NULL) {
        return mfrERR_INVALID_PARAM;
    }
#if 0 /* Enable when proper HAL Spec is available. */
    if (access(CPU_FREQ_SCALING_CUR_FREQ_PATH, F_OK | R_OK) != 0) {
        perror("Failed to access CPU frequency scaling current frequency file");
        return mfrERR_FLASH_READ_FAILED;
    }
    FILE *fp = fopen(CPU_FREQ_SCALING_CUR_FREQ_PATH, "r");
    if (fp == NULL) {
        perror("Failed to open CPU frequency scaling current frequency file");
        return mfrERR_FLASH_READ_FAILED;
    }

    int value = 0;
    if (fscanf(fp, "%d", &value) != 1) {
        perror("Failed to read CPU frequency scaling current frequency file");
        fclose(fp);
        return mfrERR_FLASH_READ_FAILED;
    }
    fclose(fp);

    // The value read is in kHz, convert it to MHz
    *speed = value / 1000;
    return mfrERR_NONE;
#else
    printf("PLAT_API_GetClockSpeed: Not implemented\n");
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif
}

#endif //ENABLE_THERMAL_PROTECTION

/**
 * @brief Terminates the CPE Power Management module
 *
 * All data structures used within Power Management module must be reset and release any
 * Power Management specific handles and resources.
 *
 * @return    pmStatus_t              - Status
 * @retval    PWRMGR_SUCCESS          - Success
 * @retval    PWRMGR_NOT_INITIALIZED  - Function is already initialized
 * @retval    PWRMGR_TERM_FAILURE     - Module failed to terminate
 *
 * @pre PLAT_INIT() must be called before calling this API
 *
 */
pmStatus_t PLAT_TERM(void)
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }

    if (pthread_mutex_lock(&power_state_mutex) != 0) {
        perror("PLAT_TERM: Failed to lock mutex");
        return PWRMGR_TERM_FAILURE;
    }

    thread_running = 0;

    if (pthread_mutex_unlock(&power_state_mutex) != 0) {
        perror("PLAT_TERM: Failed to unlock mutex");
        return PWRMGR_TERM_FAILURE;
    }

    if (sem_post(&power_state_semaphore) != 0) {
        perror("PLAT_TERM: Failed to post semaphore");
        return PWRMGR_TERM_FAILURE;
    }

    if (pthread_join(worker_thread, NULL) != 0) {
        perror("PLAT_TERM: Failed to join worker thread");
        return PWRMGR_TERM_FAILURE;
    }

    powerMgrStatus = PWRMGR_NOT_INITIALIZED;
    return PWRMGR_SUCCESS;
}

/**
 * @brief Resets the power state of the device
 *
 * @note PLAT_Reset() will be deprecated.
 *
 * @param[in] newState  - The state to be set
 *
 * @return    pmStatus_t                - Status
 * @retval    PWRMGR_SUCCESS            - Success
 * @retval    PWRMGR_NOT_INITIALIZED    - Module is not initialised
 * @retval    PWRMGR_INVALID_ARGUMENT   - Parameter passed to this function is invalid
 * @retval    PWRMGR_SET_FAILURE        - Failed to update
 *
 * @pre PLAT_INIT() must be called before calling this API
 *
 * @warning This API is Not thread safe
 *
 * @see PWRMgr_PowerState_t
 *
 */
pmStatus_t PLAT_Reset(PWRMgr_PowerState_t newState)
{
    if (PWRMGR_ALREADY_INITIALIZED != powerMgrStatus) {
        return PWRMGR_NOT_INITIALIZED;
    }

    if (!(newState >= PWRMGR_POWERSTATE_OFF && newState < PWRMGR_POWERSTATE_MAX)) {
        return PWRMGR_INVALID_ARGUMENT;
    }

    if (pthread_mutex_lock(&power_state_mutex) != 0) {
        perror("PLAT_Reset: Failed to lock mutex");
        return PWRMGR_SET_FAILURE;
    }

    thread_running = 0;

    if (pthread_mutex_unlock(&power_state_mutex) != 0) {
        perror("PLAT_Reset: Failed to unlock mutex");
        return PWRMGR_SET_FAILURE;
    }

    if (sem_post(&power_state_semaphore) != 0) {
        perror("PLAT_Reset: Failed to post semaphore");
        return PWRMGR_SET_FAILURE;
    }

    if (pthread_join(worker_thread, NULL) != 0) {
        perror("PLAT_Reset: Failed to join worker thread");
        return PWRMGR_SET_FAILURE;
    }

    if (newState == PWRMGR_POWERSTATE_OFF) {
        if (reboot(RB_POWER_OFF) != 0) {
            perror("PLAT_Reset: Failed to power off");
            return PWRMGR_SET_FAILURE;
        }
    } else {
        if (reboot(RB_AUTOBOOT) != 0) {
            perror("PLAT_Reset: Failed to reboot");
            return PWRMGR_SET_FAILURE;
        }
    }

    // Caller is not supposed to get a return from this function.
    return PWRMGR_SUCCESS;
}
