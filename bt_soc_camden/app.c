/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2024 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 * ******************************************************************************
 *
 * This package consists of the firmware for the Inflamanode Device. It supports
 * sampling from the VDAC and the generation of specific waveforms for Square Wave
 * Voltammetry, Cyclic Voltammetry, and Pulse Mode Operations. Operating parameters
 * are set using a Bluetooth connected device/app and recorded data is streamed. To
 * prevent race conditions, a rotating circular queue of sample buffers is used.
 *
 * AUTHORS:
 * Trevor D. Meyer
 * Camden A. Shultz
 * Georgia L. Lawlor
 *
 ******************************************************************************/
#include "sl_bt_api.h"
#include "gatt_db.h"
#include "sl_main_init.h"
#include "app_assert.h"
#include "app.h"

#include "em_cmu.h"
#include "em_prs.h"
#include "em_vdac.h"
#include "em_gpio.h"
#include "em_iadc.h"
#include "em_letimer.h"
#include "sl_sleeptimer.h"



//#define RUN_MODE 0 // TREVOR
//#define RUN_MODE 1 // GEORGIA V1
#define RUN_MODE 2 // GEORGIA V2

// IADC Configuration
uint16_t iadcSAMPLESperPULSE = 12; // samples
#define BLE_DATACHUNKSIZE      10;
uint16_t BLE_packetSize     = 120;
// Note: Recording frequency can theoretically supports up to 1,919 Hz
// Set CLK_ADC to 40 MHz - this will be adjusted to HFXO frequency in the initialization process
#define CLK_SRC_ADC_FREQ        40000000  // CLK_SRC_ADC - 40 MHz max
#define CLK_ADC_FREQ             5000000  // CLK_ADC - 5 MHz max in High Accuracy mode
#define ADC_TRIG_PRS_CHANNEL           0
#define ADC_REF_VOLTAGE             2.42  // 1.21 V / 0.5 multiplier = 2.42 V reference
//#define ADC_REF_VOLTAGE             1.8

// BLE Configuration
static uint8_t advertising_set_handle = 0xff;
static sl_status_t send_runExperiment_notification();
// static sl_status_t send_result_notification();
volatile bool BLE_notify_runExperiment = false;
volatile bool BLE_notify_result = false;
volatile bool BLE_transmission_busy = false;
volatile bool measurement_complete = false;
uint8_t  BLE_value_runExperiment = 0;

// BLE Packet Queue Configuration
#define BLE_QUEUE_SIZE 8  // Number of packets that can be queued
#define BLE_MAX_PACKET_SIZE 200  // Maximum size of each packet

typedef struct {
    uint8_t data[BLE_MAX_PACKET_SIZE];
    uint8_t size;  // Actual packet size
} ble_packet_t;

// Circular queue for BLE packets
ble_packet_t BLE_packet_queue[BLE_QUEUE_SIZE];
volatile uint8_t BLE_queue_head = 0;     // Points to next position to write
volatile uint8_t BLE_queue_tail = 0;     // Points to next position to read
volatile uint8_t BLE_queue_count = 0;    // Number of packets in queue

// Current packet being built
uint8_t  BLE_current_packet[BLE_MAX_PACKET_SIZE];
uint8_t  BLE_result_counter = 0; // Track current position in result buffer
uint32_t BLE_dropped_packets = 0; // Track dropped packets for debugging (should be 0 now)
uint8_t  gain_channel = 3; // Default to channel 3 (F_A1=1, F_A0=1)
uint8_t  electrode_channel = 4; // Default to channel 4 (C_A2=1, C_A1=0, C_A0=0)
uint16_t time_before_trial = 5; // Default 5 seconds before trial starts (in s)
uint16_t time_after_trial = 5;  // Default 5 seconds after trial ends (in s)
uint8_t  operating_mode = 0;    // Default to 0 (Square Wave Voltammetry), 1 = Linear Sweep, 2 = Pulse Mode
uint16_t linear_sweep_rate = 100; // Default linear sweep rate in mV/s
uint16_t linear_sweep_sample_rate = 25; // Default sampling rate for linear sweep in Hz
int16_t  linear_sweep_step = 0;   // Pre-calculated linear sweep step (in VDAC units per sample)

// Pulse mode variables
uint8_t  time_before_pulse = 1; // Default 1 second before pulse (in s)
uint8_t  time_after_pulse = 1;  // Default 1 second after pulse (in s)
uint16_t pulse_width_ms = 100;  // Default pulse width in milliseconds
uint16_t pulse_height = 40;     // Default pulse height in VDAC units (separate from vdacOUT_pulse for SWV)
uint32_t pulse_timer_count = 0; // Timer count for pulse mode timing
uint8_t  pulse_state = 0;       // 0=before_pulse, 1=pulse_active, 2=after_pulse, 3=complete
// Pre-calculated timing values (calculated once, used in interrupt)
uint32_t pulse_before_ticks = 0;    // Pre-calculated ticks for before pulse phase
uint32_t pulse_width_ticks = 0;     // Pre-calculated ticks for pulse width
uint32_t pulse_after_ticks = 0;     // Pre-calculated ticks for after pulse phase

int16_t vdacOUT_offset_volts    = -11; // in mV ##V1 device is -11, -18 for oscope, -13 for app display

// VDAC Configuration
#if RUN_MODE == 0
  #define VDAC_REF_SELECT vdacRef2V5
  #define VDAC_REF_VOLTAGE 2.5

#elif (RUN_MODE == 1) | (RUN_MODE == 2)
  #define VDAC_REF_SELECT vdacRefAvdd
  #define VDAC_REF_VOLTAGE 1.8

#endif

// VDAC Calibration - Global constant for adjusting VDAC output
// This can be used to compensate for device-specific variations
// Units: VDAC counts, positive values increase output voltage
const int16_t vdac_calibration_offset = 0;


// Application Specific Configuration
#define SWV_REF_VOLTAGE     900 // mV
uint16_t vdacOUT_offset     = 0xFFFF; // VOLTAGE_START * 4096.0 / (double) VDAC_REF_VOLTAGE;
uint16_t vdacOUT_value      = 0xFFFF; // VOLTAGE_START * 4096.0 / (double) VDAC_REF_VOLTAGE;
uint16_t vdacOUT_ref        = ((int) ((double) SWV_REF_VOLTAGE * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
uint32_t vdacOUT_count      = 0;
uint32_t iadcSAMPLE_count   = 0;
bool     iadc_isFirstSample = true;
bool     measurement_stop_requested = false;
bool     measurement_active = false;
uint32_t samples_in_current_pulse = 0;

// Linear sweep mode variables
uint32_t linear_sweep_timer_count = 0;
uint16_t linear_sweep_current_voltage = 0;
bool linear_sweep_direction_forward = true; // true = start->stop, false = stop->start

/*
 * Operating Mode Implementation:
 * Mode 0 (Square Wave Voltammetry): Uses the original pulse-based voltage changes
 * Mode 1 (Linear Sweep): Continuously sweeps voltage at linear_sweep_rate (mV/s)
 *         from start to stop voltage, taking measurements at each timer tick
 * Mode 2 (Pulse Mode): Sets voltage to START for TIME_BEFORE_PULSE, then increases
 *         by PULSE_HEIGHT for PULSE_WIDTH, then sets to STOP for TIME_AFTER_PULSE
 */



#if RUN_MODE == 0
#define VDAC_SIG_ID         VDAC0
#define VDAC_SIG_CH             0
#define VDAC_SIG_PORT vdacChPortA
#define VDAC_SIG_PIN            5
#define VDAC_SIG_BUS  GPIO_ABUSALLOC_AODD0_VDAC0CH0 // IMPORTANT Don't forget to change GPIO Register too
#define VDAC_REF_ID         VDAC1
#define VDAC_REF_CH             0
#define VDAC_REF_PORT vdacChPortC
#define VDAC_REF_PIN            6
#define VDAC_REF_BUS  GPIO_CDBUSALLOC_CDEVEN0_VDAC1CH0 // GPIO_CDBUSALLOC_CDODD0_VDAC1CH0 // IMPORTANT Don't forget to change GPIO Register too

#define IADC_INPUT_0_POS_PORT_PIN     iadcPosInputPadAna0 // Dev Kit does not need the " | 1 "
#define IADC_INPUT_1_POS_PORT_PIN     iadcPosInputPadAna2 // Dev Kit does not need the " | 1 "

#define BTN_IN_PORT gpioPortB
#define BTN_IN_PIN          3
#define LED_OUT_PORT gpioPortB
#define LED_OUT_PIN          1

#define DBG1_OUT_PORT gpioPortA
#define DBG1_OUT_PIN          7
#define DBG2_OUT_PORT gpioPortA
#define DBG2_OUT_PIN          6


#elif RUN_MODE == 1

#define VDAC_SIG_ID         VDAC0 // Square Wave VIN
#define VDAC_SIG_CH             0
#define VDAC_SIG_PORT vdacChPortA
#define VDAC_SIG_PIN            3
#define VDAC_SIG_BUS  GPIO_ABUSALLOC_AODD0_VDAC0CH0 // IMPORTANT Don't forget to change GPIO Register too
#define VDAC_REF_ID         VDAC1 // Reference
#define VDAC_REF_CH             0
#define VDAC_REF_PORT vdacChPortC
#define VDAC_REF_PIN            1
#define VDAC_REF_BUS  GPIO_CDBUSALLOC_CDODD0_VDAC1CH0 // IMPORTANT Don't forget to change GPIO Register too
//// USING PA03 Bricks the firmware, possibly liked to DBG traces. Test PA05 instead
//// ALSO PC01 is not routed out on the dev kit.  Test PC05 instead

#define IADC_INPUT_0_POS_PORT_PIN     iadcPosInputPadAna0 | 1
#define IADC_INPUT_1_POS_PORT_PIN     iadcPosInputPadAna2 | 1

#define C_A0_PORT     gpioPortB // C_A0
#define C_A0_PIN              0
#define C_A1_PORT     gpioPortA // C_A1 // DBG_TDI & DBG_TRACECLK
#define C_A1_PIN              4
#define C_A2_PORT     gpioPortA // C_A2 // DBG_TDI & DBG_TRACECLK
#define C_A2_PIN              5
#define EN_1_8_PORT   gpioPortC // 1.8V ENABLE
#define EN_1_8_PIN            3
#define EN_Vplus_PORT gpioPortA // ENABLE
#define EN_Vplus_PIN          7
#define F_A0_PORT     gpioPortB // F_A0
#define F_A0_PIN              1
#define F_A1_PORT     gpioPortB // F_A1
#define F_A1_PIN              3


#elif RUN_MODE == 2

#define VDAC_SIG_ID         VDAC0 // Square Wave VIN
#define VDAC_SIG_CH             0
#define VDAC_SIG_PORT vdacChPortA
#define VDAC_SIG_PIN            3
#define VDAC_SIG_BUS  GPIO_ABUSALLOC_AODD0_VDAC0CH0 // IMPORTANT Don't forget to change GPIO Register too
//#define VDAC_REF_ID         VDAC1 // Reference
//#define VDAC_REF_CH             0
//#define VDAC_REF_PORT vdacChPortC
//#define VDAC_REF_PIN            1
//#define VDAC_REF_BUS  GPIO_CDBUSALLOC_CDODD0_VDAC1CH0 // IMPORTANT Don't forget to change GPIO Register too
//// USING PA03 Bricks the firmware, possibly liked to DBG traces. Test PA05 instead
//// ALSO PC01 is not routed out on the dev kit.  Test PC05 instead

#define IADC_INPUT_0_POS_PORT_PIN     iadcPosInputPadAna0 | 1
#define IADC_INPUT_1_POS_PORT_PIN     iadcPosInputPadAna2 | 1

#define C_A0_PORT     gpioPortA // C_A0
#define C_A0_PIN              5
#define C_A1_PORT     gpioPortA // C_A1 // DBG_TDI & DBG_TRACECLK
#define C_A1_PIN              4
#define C_A2_PORT     gpioPortB // C_A2 // DBG_TDI & DBG_TRACECLK
#define C_A2_PIN              0
#define EN_PORT   gpioPortC // ENABLE
#define EN_PIN            3
#define F_A0_PORT     gpioPortB // F_A0
#define F_A0_PIN              1
#define F_A1_PORT     gpioPortB // F_A1
#define F_A1_PIN              3

#endif


// Initialization Values Only
#define INITIAL_VOLTAGE_START   900  // mV
#define INITIAL_VOLTAGE_STOP   1500  // mV
#define INITIAL_VOLTAGE_STEP      4  // mV
#define INITIAL_VOLTAGE_PULSE    40  // mV
#define INITIAL_PULSE_WIDTH      16  // ms
uint16_t vdacOUT_start  = ((int) ((double) INITIAL_VOLTAGE_START   * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
uint16_t vdacOUT_stop   = ((int) ((double) INITIAL_VOLTAGE_STOP    * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
 int16_t vdacOUT_step   = ((int) ((double) INITIAL_VOLTAGE_STEP    * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
 int16_t vdacOUT_pulse  = ((int) ((double) INITIAL_VOLTAGE_PULSE   * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V


// Function to apply VDAC calibration offset and set output
// This function applies the global calibration offset to compensate for device variations
// static inline void setCalibratedVdacChannelOutput(VDAC_TypeDef *vdac, unsigned int channel, uint16_t value) {
//     // Apply calibration offset with bounds checking
//     int32_t calibrated_value = (int32_t)value + vdac_calibration_offset;
    
//     // Clamp to valid VDAC range (0-4095 for 12-bit VDAC)
//     if (calibrated_value < 0) {
//         calibrated_value = 0;
//     } else if (calibrated_value > 4095) {
//         calibrated_value = 4095;
//     }
    
//     VDAC_ChannelOutputSet(vdac, channel, (uint16_t)calibrated_value);
// }

// Function to calculate linear sweep step
void calculateLinearSweepStep(void) {
    if (operating_mode == 1 && linear_sweep_sample_rate > 0) {
        // Calculate step in VDAC units per sample
        // linear_sweep_rate (mV/s) * (4.096 / VDAC_REF_VOLTAGE) / linear_sweep_sample_rate (Hz)
        double step_per_sample = (double)linear_sweep_rate * 4.096 / (double)VDAC_REF_VOLTAGE / linear_sweep_sample_rate;
        
        // Ensure minimum step size to prevent zero steps when sample rate is high
        if (step_per_sample > 0 && step_per_sample < 1.0) {
            step_per_sample = 1.0;  // Minimum step of 1 VDAC unit
        } else if (step_per_sample < 0 && step_per_sample > -1.0) {
            step_per_sample = -1.0; // Minimum step of 1 VDAC unit (negative direction)
        }
        
        // Determine direction based on start and stop voltages
        if (vdacOUT_stop >= vdacOUT_start) {
            linear_sweep_step = (int16_t)step_per_sample;
        } else {
            linear_sweep_step = -(int16_t)step_per_sample;
        }
        
        // Final safety check - ensure step is never zero when there's a voltage range to sweep
        if (linear_sweep_step == 0 && vdacOUT_start != vdacOUT_stop) {
            linear_sweep_step = (vdacOUT_stop > vdacOUT_start) ? 1 : -1;
        }
    } else {
        linear_sweep_step = 0;
    }
}

// Function to calculate pulse mode timing values (pre-compute for fast interrupt)
void calculatePulseTiming(void) {
    if (linear_sweep_sample_rate > 0) {
        // Pre-calculate all timing values to avoid calculations in interrupt
        pulse_before_ticks = (uint32_t)time_before_pulse * linear_sweep_sample_rate;
        pulse_width_ticks = (uint32_t)pulse_width_ms * linear_sweep_sample_rate / 1000;
        pulse_after_ticks = (uint32_t)time_after_pulse * linear_sweep_sample_rate;
        
        // Ensure minimum timing values
        if (pulse_width_ticks == 0) pulse_width_ticks = 1;
        if (pulse_before_ticks == 0) pulse_before_ticks = 1;
        if (pulse_after_ticks == 0) pulse_after_ticks = 1;
    }
}


void startNewMeasurement(void)
{

// Drain any pending IADC scan FIFO results to avoid processing stale samples
  while (IADC_getScanFifoCnt(IADC0) > 0) {
    (void) IADC_pullScanFifoResult(IADC0);
  }
  #if RUN_MODE == 1
  GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
  GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);

  // Set electrode channel GPIO pins based on electrode_channel value (0-7)
  // Uses 3-bit binary: C_A2 (bit 2), C_A1 (bit 1), C_A0 (bit 0)
  GPIO_PinModeSet(C_A0_PORT, C_A0_PIN, gpioModePushPull, electrode_channel & 1);
  GPIO_PinModeSet(C_A1_PORT, C_A1_PIN, gpioModePushPull, (electrode_channel >> 1) & 1);
  GPIO_PinModeSet(C_A2_PORT, C_A2_PIN, gpioModePushPull, (electrode_channel >> 2) & 1);

  // Set gain channel GPIO pins based on gain_channel value (0-3)
  // 0: F_A1=0, F_A0=0 (00 binary) - bottom (100k||10nF)
  // 1: F_A1=0, F_A0=1 (01 binary) - top (200k||1000pF)
  // 2: F_A1=1, F_A0=0 (10 binary) - middle bottom (8.22k||100nF)
  // 3: F_A1=1, F_A0=1 (11 binary) - middle top (20k||47nF)
  GPIO_PinModeSet(F_A1_PORT, F_A1_PIN, gpioModePushPull, (gain_channel >> 1) & 1);
  GPIO_PinModeSet(F_A0_PORT, F_A0_PIN, gpioModePushPull, gain_channel & 1);

#elif RUN_MODE == 2
  GPIO_PinModeSet(EN_PORT, EN_PIN, gpioModePushPull, 1);

  // Set electrode channel GPIO pins based on electrode_channel value (0-7)
  // Uses 3-bit binary: C_A2 (bit 2), C_A1 (bit 1), C_A0 (bit 0)
  GPIO_PinModeSet(C_A0_PORT, C_A0_PIN, gpioModePushPull, electrode_channel & 1);
  GPIO_PinModeSet(C_A1_PORT, C_A1_PIN, gpioModePushPull, (electrode_channel >> 1) & 1);
  GPIO_PinModeSet(C_A2_PORT, C_A2_PIN, gpioModePushPull, (electrode_channel >> 2) & 1);

  // Set gain channel GPIO pins based on gain_channel value (0-3)
  // 0: F_A1=0, F_A0=0 (00 binary) - bottom (20k)
  // 1: F_A1=0, F_A0=1 (01 binary) - top (4.7k)
  // 2: F_A1=1, F_A0=0 (10 binary) - middle bottom (12k)
  // 3: F_A1=1, F_A0=1 (11 binary) - middle top (8.2k)
  GPIO_PinModeSet(F_A1_PORT, F_A1_PIN, gpioModePushPull, (gain_channel >> 1) & 1);
  GPIO_PinModeSet(F_A0_PORT, F_A0_PIN, gpioModePushPull, gain_channel & 1);
#endif

//  VDAC_ChannelOutputSet(VDAC_REF_ID, VDAC_REF_CH, vdacOUT_ref);
  VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_start);

//  sl_sleeptimer_delay_millisecond(30000); // Delay for X milliseconds
  sl_sleeptimer_delay_millisecond(time_before_trial * 1000); // Configurable delay before trial starts

//  VDAC_ChannelOutputSet(VDAC_REF_ID, VDAC_REF_CH, vdacOUT_ref);

  if (vdacOUT_offset == 0xFFFF) {
      vdacOUT_offset = vdacOUT_start;

      iadcSAMPLE_count = 0;
      vdacOUT_count  = 0;
      iadc_isFirstSample = true;
      measurement_stop_requested = false;
      measurement_complete = false;
      measurement_active = true;
      samples_in_current_pulse = 0;
      BLE_result_counter = 0; // Reset result counter for new measurement
      BLE_dropped_packets = 0; // Reset dropped packet counter
      BLE_transmission_busy = false; // Reset transmission busy flag
      // Reset queue
      BLE_queue_head = 0;
      BLE_queue_tail = 0;
      BLE_queue_count = 0;
      BLE_value_runExperiment = 1;
      BLE_notify_runExperiment = true;
      
      // Initialize linear sweep variables
      linear_sweep_timer_count = 0;
      linear_sweep_current_voltage = vdacOUT_start;
      linear_sweep_direction_forward = true; // Always start going forward
      
      // Calculate linear sweep step and pulse timing
      calculateLinearSweepStep();
      calculatePulseTiming();

      if (operating_mode == 0) {
          uint32_t topValue = (int) ((double) pulse_width_ms * 32.768 / iadcSAMPLESperPULSE);
          LETIMER_TopSet(LETIMER0, topValue);
          LETIMER_CounterSet(LETIMER0, topValue);
      } else if (operating_mode == 1) {
          BLE_packetSize = 200;
          // For linear sweep mode, set timer frequency to match sampling rate
          uint32_t topValue = (uint32_t)(32768.0 / linear_sweep_sample_rate);
          LETIMER_TopSet(LETIMER0, topValue);
          LETIMER_CounterSet(LETIMER0, topValue);
          // Set initial voltage for linear sweep
          vdacOUT_value = vdacOUT_start;
      } else if (operating_mode == 2) {
          // For pulse mode, use linear_sweep_sample_rate to set timer frequency
          BLE_packetSize = 200;
          uint32_t topValue = (uint32_t)(32768.0 / linear_sweep_sample_rate);
          LETIMER_TopSet(LETIMER0, topValue);
          LETIMER_CounterSet(LETIMER0, topValue);
          // Initialize pulse mode variables
          pulse_timer_count = 0;
          pulse_state = 0; // Start in before_pulse state
          vdacOUT_value = vdacOUT_start; // Set initial voltage to start voltage
      }
      
      LETIMER_Enable(LETIMER0, true); // Start the timer

#if RUN_MODE == 0
      GPIO_PinOutClear(LED_OUT_PORT, LED_OUT_PIN);
#endif

  } // else { // Test is already running, do nothing
}

void stopThisMeasurement() {
  BLE_value_runExperiment  = 0;
  BLE_notify_runExperiment = true;
  measurement_active = false;

  // Send any remaining partial data before stopping
  // if (BLE_result_counter > 0) {
  //   // Store the final packet size for send_result_notification to use
  //   BLE_final_packet_size = BLE_result_counter;
  //   BLE_result_counter = 0; // Reset counter
  //   BLE_result_select = !BLE_result_select; // Switch buffers
  //   BLE_notify_result = true; // Trigger notification with partial data
    
  //   // Wait a bit to ensure the notification is sent
  //   sl_sleeptimer_delay_millisecond(50);
    
  //   // Reset final packet size
  //   BLE_final_packet_size = 0;
  // }

  vdacOUT_value = vdacOUT_ref;
  VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
  vdacOUT_offset = 0xFFFF;

  // Configurable delay after trial ends
  // sl_sleeptimer_delay_millisecond(time_after_trial * 1000);


  #if RUN_MODE == 0
    GPIO_PinOutSet(LED_OUT_PORT, LED_OUT_PIN);

  #elif RUN_MODE == 1
    GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
    GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);

  #elif RUN_MODE == 2
    GPIO_PinModeSet(EN_PORT, EN_PIN, gpioModePushPull, 1);
  #endif
}

// Queue management functions
static bool BLE_queue_is_full(void) {
    return BLE_queue_count >= BLE_QUEUE_SIZE;
}

static bool BLE_queue_is_empty(void) {
    return BLE_queue_count == 0;
}

static bool BLE_enqueue_packet(uint8_t *data, uint8_t size) {
    if (BLE_queue_is_full()) {
        BLE_dropped_packets++;
        return false; // Queue is full
    }
    
    // Copy data to queue
    for (int i = 0; i < size && i < BLE_MAX_PACKET_SIZE; i++) {
        BLE_packet_queue[BLE_queue_head].data[i] = data[i];
    }
    BLE_packet_queue[BLE_queue_head].size = size;
    
    // Update head pointer
    BLE_queue_head = (BLE_queue_head + 1) % BLE_QUEUE_SIZE;
    BLE_queue_count++;
    
    return true; // Successfully enqueued
}

static bool BLE_dequeue_packet(uint8_t *data, uint8_t *size) {
    if (BLE_queue_is_empty()) {
        return false; // Queue is empty
    }
    
    // Copy data from queue
    *size = BLE_packet_queue[BLE_queue_tail].size;
    for (int i = 0; i < *size; i++) {
        data[i] = BLE_packet_queue[BLE_queue_tail].data[i];
    }
    
    // Update tail pointer
    BLE_queue_tail = (BLE_queue_tail + 1) % BLE_QUEUE_SIZE;
    BLE_queue_count--;
    
    return true; // Successfully dequeued
}

void IADC_IRQHandler(void)
{
  IADC_Result_t sample;
  uint32_t result_channel0 = 0;
  uint32_t result_channel1 = 0;
  static uint32_t last_processed_count = 0xFFFFFFFF; // Track last processed sample count
  bool ch0_received = false;
  bool ch1_received = false;

  // Clear interrupt first to prevent re-entrance
  IADC_clearInt(IADC0, IADC_IEN_SCANTABLEDONE);

  // Check if measurement is active before processing
  if (!measurement_active) {
    return;
  }

  // Prevent processing duplicate samples for the same count
  if (iadcSAMPLE_count == last_processed_count) {
    return; // Already processed this sample count
  }

  if (iadc_isFirstSample) {
      iadc_isFirstSample = false;
//      vdacOUT_value = vdacOUT_offset + vdacOUT_pulse;
//      VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value); // TDM MAYBE THIS???
  } else {

    while (IADC_getScanFifoCnt(IADC0)) {
          // Pull a scan result from the FIFO
          sample = IADC_pullScanFifoResult(IADC0);

          if (sample.id == 0) {
              result_channel0 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
          } else if (sample.id == 1) {
              result_channel1 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
          }
        }
    IADC_command(IADC0, iadcCmdStopScan);

    // // Increment samples in current pulse counter
    samples_in_current_pulse++;

    // // While both channels have not been received and FIFO has data
    // while (!(ch0_received && ch1_received) && IADC_getScanFifoCnt(IADC0)) {
    //   // Pull a scan result from the FIFO
    //   sample = IADC_pullScanFifoResult(IADC0);

    //   if (sample.id == 0) {
    //       #if RUN_MODE == 0
    //       //          GPIO_PinOutSet(DBG1_OUT_PORT, DBG1_OUT_PIN);
    //                 //double sample_voltage = sample.data * 2.42 / 0xFFF;
    //       #endif
    //       result_channel0 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
    //       ch0_received = true;
    //   } else if (sample.id == 1) {
    //       #if RUN_MODE == 0
    //       //          GPIO_PinOutSet(DBG2_OUT_PORT, DBG2_OUT_PIN);
    //                 //double sample_voltage = sample.data * 2.42 / 0xFFF;
    //       #endif
    //       result_channel1 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
    //       ch1_received = true;
    //   }
    // }

    // Only construct and send packet if we have data from both channels
    // if (ch0_received && ch1_received) {
      // Update last processed count to prevent duplicates
      last_processed_count = iadcSAMPLE_count;

      // Construct Packet in current packet buffer
      BLE_current_packet[BLE_result_counter+0] = ( result_channel0 & 0x0000FF)      ;
      BLE_current_packet[BLE_result_counter+1] = ( result_channel0 & 0x00FF00) >>  8;
      BLE_current_packet[BLE_result_counter+2] = ( result_channel0 & 0xFF0000) >> 16;
      BLE_current_packet[BLE_result_counter+3] = ( result_channel1 & 0x0000FF)      ;
      BLE_current_packet[BLE_result_counter+4] = ( result_channel1 & 0x00FF00) >>  8;
      BLE_current_packet[BLE_result_counter+5] = ( result_channel1 & 0xFF0000) >> 16;
      BLE_current_packet[BLE_result_counter+6] = (   vdacOUT_value & 0x0000FF)      ;
      BLE_current_packet[BLE_result_counter+7] = (   vdacOUT_value & 0x00FF00) >>  8;
      BLE_current_packet[BLE_result_counter+8] = (iadcSAMPLE_count & 0x0000FF)      ;
      BLE_current_packet[BLE_result_counter+9] = (iadcSAMPLE_count & 0x00FF00) >>  8;

      BLE_result_counter += BLE_DATACHUNKSIZE;
      if (BLE_result_counter >= BLE_packetSize) {
          // Packet is complete, enqueue it
          if (BLE_enqueue_packet(BLE_current_packet, BLE_packetSize)) {
            // Successfully enqueued, trigger BLE transmission if not already busy
            if (!BLE_notify_result) {
              BLE_notify_result = true;
            }
          }
          // Reset counter for next packet regardless of enqueue success
          BLE_result_counter = 0;
      }

      // Check if we need to stop measurement after completing the current pulse
      if (measurement_stop_requested && (samples_in_current_pulse >= iadcSAMPLESperPULSE)) {
        // All samples for the current pulse have been collected, safe to signal completion
        measurement_complete = true;
        measurement_stop_requested = false;
        samples_in_current_pulse = 0;
      }
    // } else {
    //   // Safety check: if stop was requested but we're not getting samples normally,
    //   // stop anyway to prevent hanging (should not normally happen)
    //   if (measurement_stop_requested) {
    //     measurement_complete = true;
    //     measurement_stop_requested = false;
    //     samples_in_current_pulse = 0;
    //   }
    // }

  }

#if RUN_MODE == 0
  GPIO_PinOutClear(DBG1_OUT_PORT, DBG1_OUT_PIN);
//  GPIO_PinOutClear(DBG2_OUT_PORT, DBG2_OUT_PIN);
#endif
}


 // MORE LIKE THE ORIGINAL TREVOR VERSION BELOW

// void LETIMER0_IRQHandler(void)
// {
//   uint32_t flags = LETIMER_IntGet(LETIMER0);

//   if (!measurement_active) {
//     // Clear interrupt and return without processing
//     LETIMER_IntClear(LETIMER0, flags);
//     return;
//   }

//   if (flags & 0x1) {
//       iadcSAMPLE_count++;
//       // Trigger an IADC scan conversion
//       IADC_command(IADC0, iadcCmdStartScan);
// #if RUN_MODE == 0
//       GPIO_PinOutSet(DBG1_OUT_PORT, DBG1_OUT_PIN);
// #endif

//   } else {
// #if RUN_MODE == 0
//           GPIO_PinOutSet(DBG2_OUT_PORT, DBG2_OUT_PIN);
// #endif
    

//     if ((iadcSAMPLE_count % iadcSAMPLESperPULSE) == 0) {
//           vdacOUT_count++;

//           if ( vdacOUT_count & 0x1) {
//             vdacOUT_value = vdacOUT_offset - vdacOUT_pulse;
//             VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
//           } else {
//             vdacOUT_offset += vdacOUT_step;
//             if (((vdacOUT_step > 0) && (vdacOUT_offset <= vdacOUT_stop)) ||
//                 ((vdacOUT_step < 0) && (vdacOUT_offset >= vdacOUT_stop)) ||
//                 (vdacOUT_step == 0) ) {
//               vdacOUT_value = vdacOUT_offset + vdacOUT_pulse;
//               VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
//             } else {
//               // Request stop after current pulse completes instead of stopping immediately
//                 stopThisMeasurement();
//             }
//           }
//         }
//   }

// #if RUN_MODE == 0
//           GPIO_PinOutClear(DBG2_OUT_PORT, DBG2_OUT_PIN);
// #endif
//   // Clear LETIMER interrupt flags
//   LETIMER_IntClear(LETIMER0, flags);
// }


// CAMDEN's MODIFIED VERSION BELOW

void LETIMER0_IRQHandler(void)
{
  uint32_t flags = LETIMER_IntGet(LETIMER0);

  // Check if measurement is active before processing
  if (!measurement_active) {
    // Clear interrupt and return without processing
    LETIMER_IntClear(LETIMER0, flags);
    return;
  }

  if (flags & 0x1) {
      iadcSAMPLE_count++;
      // Trigger an IADC scan conversion (common for all modes)
      IADC_command(IADC0, iadcCmdStartScan);
#if RUN_MODE == 0
      GPIO_PinOutSet(DBG1_OUT_PORT, DBG1_OUT_PIN);
#endif

  } else {
#if RUN_MODE == 0
          GPIO_PinOutSet(DBG2_OUT_PORT, DBG2_OUT_PIN);
#endif

      if (operating_mode == 0) {
        if ((iadcSAMPLE_count % iadcSAMPLESperPULSE) == 0) {
          vdacOUT_count++;

          if ( vdacOUT_count & 0x1) {
            vdacOUT_value = vdacOUT_offset - vdacOUT_pulse;
            VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
          } else {
            vdacOUT_offset += vdacOUT_step;
            if (((vdacOUT_step > 0) && (vdacOUT_offset <= vdacOUT_stop)) ||
                ((vdacOUT_step < 0) && (vdacOUT_offset >= vdacOUT_stop)) ||
                (vdacOUT_step == 0) ) {
              vdacOUT_value = vdacOUT_offset + vdacOUT_pulse;
              VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
            } else {
              // Request stop after current pulse completes instead of stopping immediately
                measurement_stop_requested = true;
            }
          }

          // Reset samples counter for new pulse
          // samples_in_current_pulse = 0;
        }
      } else if (operating_mode == 1) {
        // Linear Sweep Mode - bidirectional sweep (start->stop->start)
        int16_t current_step = linear_sweep_step;
        
        // Reverse step direction if going backwards
        if (!linear_sweep_direction_forward) {
          current_step = -current_step;
        }
        
        // Update voltage by current step
        vdacOUT_value += current_step;
        
        // Check if we've reached an endpoint and need to reverse direction
        bool endpoint_reached = false;
        if (linear_sweep_direction_forward) {
          // Going forward (start -> stop)
          if ((current_step > 0 && vdacOUT_value >= vdacOUT_stop) ||
              (current_step < 0 && vdacOUT_value <= vdacOUT_stop)) {
            vdacOUT_value = vdacOUT_stop;
            linear_sweep_direction_forward = false; // Start going backwards
            endpoint_reached = true;
          }
        } else {
          // Going backward (stop -> start)
          if ((current_step > 0 && vdacOUT_value >= vdacOUT_start) ||
              (current_step < 0 && vdacOUT_value <= vdacOUT_start)) {
            vdacOUT_value = vdacOUT_start;
            // Complete cycle - stop measurement or start new cycle
            measurement_stop_requested = true;
            endpoint_reached = true;
          }
        }
        
        // Update VDAC output
        VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
      } else if (operating_mode == 2) {
        // Pulse Mode - fast state machine using pre-calculated timing values
        pulse_timer_count++;
        
        switch (pulse_state) {
          case 0: // Before pulse phase
            vdacOUT_value = vdacOUT_start;
            if (pulse_timer_count >= pulse_before_ticks) {
              pulse_state = 1; // Move to pulse active phase
              pulse_timer_count = 0; // Reset timer for next phase
              // Calculate pulse voltage: start + pulse_height
              vdacOUT_value = vdacOUT_start + pulse_height;
            }
            break;
            
          case 1: // Pulse active phase
            // Voltage is already set to start + pulse_height from previous state
            if (pulse_timer_count >= pulse_width_ticks) {
              pulse_state = 2; // Move to after pulse phase
              pulse_timer_count = 0; // Reset timer for next phase
              vdacOUT_value = vdacOUT_stop; // Set to stop voltage
            }
            break;
            
          case 2: // After pulse phase
            vdacOUT_value = vdacOUT_stop;
            if (pulse_timer_count >= pulse_after_ticks) {
              pulse_state = 3; // Mark as complete
              measurement_stop_requested = true;
            }
            break;
            
          case 3: // Complete - measurement should stop
          default:
            measurement_stop_requested = true;
            break;
        }
        
        // Update VDAC output
        VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
      }
  }

#if RUN_MODE == 0
          GPIO_PinOutClear(DBG2_OUT_PORT, DBG2_OUT_PIN);
#endif
  // Clear LETIMER interrupt flags
  LETIMER_IntClear(LETIMER0, flags);
}


/**************************************************************************//**
 * @brief
 *    VDAC initialization
 *****************************************************************************/
void initVdac(void)
{
  // Use default settings
  VDAC_Init_TypeDef        initSig        = VDAC_INIT_DEFAULT;
//  VDAC_Init_TypeDef        initRef        = VDAC_INIT_DEFAULT;
  VDAC_InitChannel_TypeDef initChannelSig = VDAC_INITCHANNEL_DEFAULT;
//  VDAC_InitChannel_TypeDef initChannelRef = VDAC_INITCHANNEL_DEFAULT;

  // The EM01GRPACLK is chosen as VDAC clock source since the VDAC will be
  // operating in EM1
  // Options: cmuSelect_FSRCO, cmuSelect_HFRCOEM23, cmuSelect_EM01GRPACLK, cmuSelect_EM23GRPACLK
  // If the VDAC is to be operated in EM2 or EM3, VDACn_CLK must be configured to use either HFRCOEM23, EM23GRPACLK or FSRCO instead of the EM01GRPACLK clock.
  // HFRCOEM23 is generally recommended for EM2/EM3 operation
  CMU_ClockSelectSet(cmuClock_VDAC0, cmuSelect_EM01GRPACLK);
//  CMU_ClockSelectSet(cmuClock_VDAC1, cmuSelect_EM01GRPACLK);

  // Enable the VDAC clocks
  CMU_ClockEnable(cmuClock_VDAC0, true);
//  CMU_ClockEnable(cmuClock_VDAC1, true);
  //CMU_ClockEnable(cmuClock_PRS, true);
  /*
    * Note: For EFR32xG21 radio devices, library function calls to
    * CMU_ClockEnable() have no effect as oscillators are automatically turned
    * on/off based on demand from the peripherals; CMU_ClockEnable() is a dummy
    * function for EFR32xG21 for library consistency/compatibility.
    *
    * Also note that there's no VDAC peripheral on xG21 and xG22
    */

  // Calculate the VDAC clock prescaler value resulting in a 1 MHz VDAC clock.
  initSig.prescaler    = VDAC_PrescaleCalc(VDAC_SIG_ID, (uint32_t) 1000000);
  initSig.reference    = VDAC_REF_SELECT;
  initSig.biasKeepWarm = true; // Set to true if iADC is sharing an internal reference voltage. Costs ~4 uA
  initSig.diff         = false;

//  initRef.prescaler    = VDAC_PrescaleCalc(VDAC_REF_ID, (uint32_t) 1000000);
//  initRef.reference    = VDAC_REF_SELECT;
//  initRef.biasKeepWarm = true; // Set to true if iADC is sharing an internal reference voltage. Costs ~4 uA
//  initRef.diff         = false;

  // Since the minimum load requirement for high capacitance mode is 25 nF, turn
  // this mode off
  initChannelSig.highCapLoadEnable = false;
  initChannelSig.powerMode = vdacPowerModeHighPower;

  initChannelSig.sampleOffMode = false; // false indicates continuous conversion mode
  initChannelSig.holdOutTime   = 0;     // Set to zero in continuous mode
  initChannelSig.warmupKeepOn  = true;  // Set to true if both channels used to reduce kickback

  initChannelSig.trigMode = vdacTrigModeSw;
  // initChannel.trigMode = vdacTrigModeAsyncPrs; // Therefore triggered by prsConsumerVDAC0_ASYNCTRIGCH0 in LETIMER
  // vdacTrigModeSyncPrs, vdacTrigModeAsyncPrs

  initChannelSig.enable        = true;
  initChannelSig.mainOutEnable = false;
  initChannelSig.auxOutEnable  = true;
  initChannelSig.shortOutput   = false;


//  // Since the minimum load requirement for high capacitance mode is 25 nF, turn
//  // this mode off
//  initChannelRef.highCapLoadEnable = false;
//  initChannelRef.powerMode = vdacPowerModeHighPower;
//
//  initChannelRef.sampleOffMode = false; // false indicates continuous conversion mode
//  initChannelRef.holdOutTime   = 0;     // Set to zero in continuous mode
//  initChannelRef.warmupKeepOn  = true;  // Set to true if both channels used to reduce kickback
//
//  initChannelRef.trigMode = vdacTrigModeSw;
//  // initChannel.trigMode = vdacTrigModeAsyncPrs; // Therefore triggered by prsConsumerVDAC0_ASYNCTRIGCH0 in LETIMER
//  // vdacTrigModeSyncPrs, vdacTrigModeAsyncPrs
//
//  initChannelRef.enable        = true;
//  initChannelRef.mainOutEnable = false;
//  initChannelRef.auxOutEnable  = true;
//  initChannelRef.shortOutput   = false;


  // Enable the VDAC SIGNAL

  initChannelSig.port = VDAC_SIG_PORT;
  initChannelSig.pin  = VDAC_SIG_PIN;
  GPIO->ABUSALLOC     = VDAC_SIG_BUS;
//  GPIO->CDBUSALLOC    = VDAC_SIG_BUS;
//  GPIO->BBUSALLOC     = VDAC_SIG_BUS;

  VDAC_Init(        VDAC_SIG_ID, &initSig);
  VDAC_InitChannel( VDAC_SIG_ID, &initChannelSig, VDAC_SIG_CH);
  VDAC_Enable(      VDAC_SIG_ID, VDAC_SIG_CH, true);

  // Enable the VDAC REFERENCE

//  initChannelRef.port = VDAC_REF_PORT;
//  initChannelRef.pin  = VDAC_REF_PIN;
////  GPIO->ABUSALLOC     = VDAC_REF_BUS;
//  GPIO->CDBUSALLOC    = VDAC_REF_BUS;
////  GPIO->BBUSALLOC     = VDAC_REF_BUS;
//
//  VDAC_Init(        VDAC_REF_ID, &initRef);
//  VDAC_InitChannel( VDAC_REF_ID, &initChannelRef, VDAC_REF_CH);
//  VDAC_Enable(      VDAC_REF_ID, VDAC_REF_CH, true);

}



void initIADC(void)
{
  IADC_Init_t       IADCconfig_init           = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t IADCconfig_initAllConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitScan_t   IADCconfig_initScan       = IADC_INITSCAN_DEFAULT;
  IADC_ScanTable_t  IADCconfig_scanTable      = IADC_SCANTABLE_DEFAULT;

  CMU_ClockEnable(cmuClock_IADC0, true);

  // Use the EM01GRPACLK as the IADC clock
  CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_EM01GRPACLK);

  // Shutdown between conversions to reduce current
  IADCconfig_init.warmup = iadcWarmupNormal;
  IADCconfig_init.iadcClkSuspend0 = true;
  // Set the HFSCLK prescale value here
  IADCconfig_init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, CLK_SRC_ADC_FREQ, 0);

  // Use internal bandgap as the reference and specify the reference voltage in mV.
  IADCconfig_initAllConfigs.configs[0].reference  = iadcCfgReferenceInt1V2;
  //initAllConfigs.configs[0].reference = iadcCfgReferenceVddx;
  IADCconfig_initAllConfigs.configs[0].vRef       = 1210;
  IADCconfig_initAllConfigs.configs[0].analogGain = iadcCfgAnalogGain0P5x; // 1.21 V / 0.5 multiplier = 2.42 V reference

  // Set the accuracy mode via over-sampling ratio.
  // Sample rate must be slow enough to support your selection here.
  // Note ADC_CLK is probably 5 MHz (max in high-accuracy mode)
  // Conversion time = (5us warm-up) + numScanCannels * ((5*OSR + 7) / freq_ADC_CLK)  - for HighAccuracy Mode
  IADCconfig_initAllConfigs.configs[0].adcMode         = iadcCfgModeHighAccuracy;    // iadcCfgModeNormal ; iadcCfgModeHighAccuracy ; iadcCfgModeHighSpeed
  IADCconfig_initAllConfigs.configs[0].osrHighAccuracy = iadcCfgOsrHighAccuracy64x; // 5 MHz CLK_ADC --> 0.258 ms per Sample, total = 5 us + 2ch * 258us =  0.521 ms
  // Additional Digital Averaging, only use this if you have already max out over-sampling ratio
  IADCconfig_initAllConfigs.configs[0].digAvg = iadcDigitalAverage1; // iadcDigitalAverage1 = no averaging

  // CLK_SRC_ADC must be prescaled by some value greater than 1 to derive the intended CLK_ADC frequency.
  IADCconfig_initAllConfigs.configs[0].adcClkPrescale = IADC_calcAdcClkPrescale(IADC0, CLK_ADC_FREQ, 0, // IADC Instance, ADC Clk Frea, CMU Clk Freq (copied from IADC_calcSrcClkPrescale() above)
                                                                     iadcCfgModeHighAccuracy, // ADC Mode
                                                                     IADCconfig_init.srcClkPrescale);    // srcClkPrescaler

  // The IADC local timer triggers conversions (scan of multiple channels).
//  IADCconfig_initScan.triggerSelect = iadcTriggerSelTimer;
//  IADCconfig_initScan.triggerSelect = iadcTriggerSelPrs0PosEdge;
  IADCconfig_initScan.triggerSelect = iadcTriggerSelImmediate;
  IADCconfig_initScan.triggerAction = iadcTriggerActionOnce; // or continuous
  IADCconfig_initScan.showId        = true;
  IADCconfig_initScan.start         = false;

  // Make sure to get all of the ADC resolution
  IADCconfig_initScan.alignment = iadcAlignRight20; // Right12 is default

  // Not Used until DMA activated
  IADCconfig_initScan.dataValidLevel = iadcFifoCfgDvl2;
  IADCconfig_initScan.fifoDmaWakeup  = false;

  /*
   * Configure entries in the scan table.  CH0 is single-ended from
   * input 0; CH1 is single-ended from input 1.
   */
  IADCconfig_scanTable.entries[0].posInput      = IADC_INPUT_0_POS_PORT_PIN;
  IADCconfig_scanTable.entries[0].negInput      = iadcNegInputGnd;
  IADCconfig_scanTable.entries[0].includeInScan = true;

  IADCconfig_scanTable.entries[1].posInput      = IADC_INPUT_1_POS_PORT_PIN;
  IADCconfig_scanTable.entries[1].negInput      = iadcNegInputGnd;
  IADCconfig_scanTable.entries[1].includeInScan = true;

  // Initialize IADC
  IADC_init(IADC0, &IADCconfig_init, &IADCconfig_initAllConfigs);

  // Initialize scan
  IADC_initScan(IADC0, &IADCconfig_initScan, &IADCconfig_scanTable);

  // Enable the IADC timer (must be after the IADC is initialized)
  IADC_command(IADC0, iadcCmdEnableTimer);

  // Allocate the analog bus for ADC0 inputs
  // Not necessary if using dedicated ADC pins
  //GPIO->IADC_INPUT_0_BUS |= IADC_INPUT_0_BUSALLOC;
  //GPIO->IADC_INPUT_1_BUS |= IADC_INPUT_1_BUSALLOC;

  // Enable scan interrupts
  IADC_enableInt(IADC0, IADC_IEN_SCANTABLEDONE);
  
  // Set IADC interrupt priority (lower number = higher priority)
  // IADC should have lower priority than LETIMER to avoid conflicts
  NVIC_SetPriority(IADC_IRQn, 1);

  // Enable ADC interrupts
  NVIC_ClearPendingIRQ(IADC_IRQn);
  NVIC_EnableIRQ(IADC_IRQn);
}

void initTimer(void) {
  // CUM_ClockSelectSet( cmuClock_LETIMER0, cmuSelect);

  CMU_ClockEnable(cmuClock_LETIMER0, true);
  // CMU_ClockEnable(cmuClock_PRS, true);

  LETIMER_Init_TypeDef init = LETIMER_INIT_DEFAULT;

  init.enable  = false;   // Don't start once finished
  init.repMode = letimerRepeatFree;

 /*
  // Enable LETIMER0 output0
  GPIO->LETIMERROUTE.ROUTEEN = GPIO_LETIMER_ROUTEEN_OUT0PEN;
  GPIO->LETIMERROUTE.OUT0ROUTE = \
      (DBG1_OUT_PORT << _GPIO_LETIMER_OUT0ROUTE_PORT_SHIFT) \
    | (DBG1_OUT_PIN  << _GPIO_LETIMER_OUT0ROUTE_PIN_SHIFT);
*/

  LETIMER_Init(LETIMER0, &init); // Write to CTRL register

  uint32_t topValue = (int) ((double) INITIAL_PULSE_WIDTH * 32.768 / iadcSAMPLESperPULSE);
  LETIMER_TopSet(LETIMER0, topValue);

  LETIMER_CompareSet(LETIMER0, 0, 18); // 18 / 32,768 = 0.549 ms > 0.521 ms ADC Sample


  //PRS_ConnectSignal(   PRS_CHANNEL, prsTypeAsync, prsSignalLETIMER0_CH0);
  //PRS_ConnectConsumer( PRS_CHANNEL, prsTypeAsync, prsConsumerVDAC0_ASYNCTRIGCH0);

  // Enable underflow interrupts
  LETIMER_IntEnable(LETIMER0, LETIMER_IEN_UF);
  LETIMER_IntEnable(LETIMER0, LETIMER_IEN_COMP0);
  
  // Set LETIMER interrupt priority (lower number = higher priority)
  // LETIMER should have higher priority than IADC for timing accuracy
  NVIC_SetPriority(LETIMER0_IRQn, 1);

  // Enable LETIMER interrupts
  NVIC_ClearPendingIRQ(LETIMER0_IRQn);
  NVIC_EnableIRQ(LETIMER0_IRQn);
}


//void initPRS(void) {
//  // Use LETIMER0 as async PRS to trigger IADC in EM2
//  CMU_ClockEnable(cmuClock_PRS, true);
//
//  /* Set up PRS LETIMER and IADC as producer and consumer respectively */
//  PRS_SourceAsyncSignalSet(ADC_TRIG_PRS_CHANNEL, PRS_ASYNC_CH_CTRL_SOURCESEL_LETIMER0, PRS_LETIMER0_CH0);
//  PRS_ConnectConsumer(     ADC_TRIG_PRS_CHANNEL, prsTypeAsync, prsConsumerIADC0_SCANTRIGGER);
//}



void initGPIO(void)
{
  // From example, not sure what LFA is
  // CMU_ClockSelectSet(cmuClock_LFA, cmuSelect_ULFRCO);

  CMU_ClockEnable(cmuClock_GPIO,true);
  //CMU_ClockEnable(cmuClock_PRS,true);

#if RUN_MODE == 0
  GPIO_PinModeSet( BTN_IN_PORT,  BTN_IN_PIN,   gpioModeInput,    0);
  GPIO_PinModeSet( LED_OUT_PORT, LED_OUT_PIN,  gpioModePushPull, 1);

  GPIO_PinModeSet(DBG1_OUT_PORT, DBG1_OUT_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(DBG2_OUT_PORT, DBG2_OUT_PIN, gpioModePushPull, 0);

#elif RUN_MODE == 1
  // C_A0, C_A1, C_A2 pins will be set dynamically in startNewMeasurement based on electrode_channel

  GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
  GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);

  // F_A1 and F_A0 pins will be set dynamically in startNewMeasurement based on gain_channel

#elif RUN_MODE == 2
  // C_A0, C_A1, C_A2 pins will be set dynamically in startNewMeasurement based on electrode_channel

  GPIO_PinModeSet(EN_PORT, EN_PIN, gpioModePushPull, 1);

  // F_A1 and F_A0 pins will be set dynamically in startNewMeasurement based on gain_channel
#endif
}


// Application Init.
void app_init(void)
{
  sl_status_t status;
  status = sl_sleeptimer_init(); // Initialize the sleeptimer
  if(status != SL_STATUS_OK) {
      // Handle error
  }

  initVdac();
  initGPIO();
//  initPRS();
  initIADC();
  initTimer();

  vdacOUT_value = vdacOUT_ref;
  VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
//  VDAC_ChannelOutputSet(VDAC_REF_ID, VDAC_REF_CH, vdacOUT_ref);
}

// Application Process Action.
void app_process_action(void)
{
  if (app_is_process_required()) {
  }

  // Handle measurement completion in main loop context (not interrupt context)
  if (measurement_complete) {
    measurement_complete = false;
    stopThisMeasurement();
  }

  if (BLE_notify_runExperiment) {
    sl_status_t sc = send_runExperiment_notification();
    if (sc == SL_STATUS_OK) {
        BLE_notify_runExperiment = false;
    }
    // If notification fails, keep the flag set to retry
  }

  if (BLE_notify_result && !BLE_transmission_busy) {
      // Try to dequeue and send a packet
      uint8_t packet_data[BLE_MAX_PACKET_SIZE];
      uint8_t packet_size;
      
      if (BLE_dequeue_packet(packet_data, &packet_size)) {
          BLE_transmission_busy = true;
          sl_status_t sc = sl_bt_gatt_server_notify_all(gattdb_ADC_RESULT, packet_size, packet_data);
          
          if (sc == SL_STATUS_OK) {
              BLE_transmission_busy = false; // Clear busy flag after successful transmission
              
              // Check if there are more packets to send
              if (!BLE_queue_is_empty()) {
                  // Keep BLE_notify_result true to continue processing queue
              } else {
                  BLE_notify_result = false; // No more packets to send
              }
          } else {
              // Transmission failed, put the packet back at the front of the queue
              // For simplicity, we'll just set busy to false and retry next time
              BLE_transmission_busy = false;
              // The packet is already dequeued, so we lost it. 
              // In a more sophisticated system, we could implement a "front insert" operation
          }
      } else {
          // Queue is empty
          BLE_notify_result = false;
      }
  }
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the default weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:

      // Initialize Values
      uint16_t vdac_ref_voltage = VDAC_REF_VOLTAGE * 1000;
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_VDAC_REF_GATT,
                                                   0, sizeof(vdac_ref_voltage), &vdac_ref_voltage);
      uint16_t iadc_ref_voltage = ADC_REF_VOLTAGE * 1000;
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_IADC_REF_GATT,
                                                   0, sizeof(iadc_ref_voltage), &iadc_ref_voltage);

      // Initialize gain channel to default value (3)
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_GAIN_CHANNEL,
                                                   0, sizeof(gain_channel), &gain_channel);

      // Initialize electrode channel to default value (4)
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_ELECTRODE_CHANNEL,
                                                   0, sizeof(electrode_channel), &electrode_channel);

      // Initialize timing values to default
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_TIME_BEFORE_TRIAL,
                                                   0, sizeof(time_before_trial), &time_before_trial);
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_TIME_AFTER_TRIAL,
                                                   0, sizeof(time_after_trial), &time_after_trial);

      // Initialize operating mode to default (Square Wave Voltammetry)
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_OPERATING_MODE,
                                                   0, sizeof(operating_mode), &operating_mode);

      // Initialize linear sweep rate to default
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_LINEAR_SWEEP_RATE,
                                                   0, sizeof(linear_sweep_rate), &linear_sweep_rate);

      // Initialize linear sweep sample rate to default
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_LINEAR_SWEEP_SAMPLE_RATE,
                                                   0, sizeof(linear_sweep_sample_rate), &linear_sweep_sample_rate);

      // Initialize pulse mode timing parameters to default
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_TIME_BEFORE_PULSE,
                                                   0, sizeof(time_before_pulse), &time_before_pulse);
      sc = sl_bt_gatt_server_write_attribute_value(gattdb_TIME_AFTER_PULSE,
                                                   0, sizeof(time_after_pulse), &time_after_pulse);

      // Create an advertising set.
      sc = sl_bt_advertiser_create_set(&advertising_set_handle);

      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);

      // Set advertising interval to 100ms.
      sc = sl_bt_advertiser_set_timing(
        advertising_set_handle,
        160, // min. adv. interval (milliseconds * 1.6)
        160, // max. adv. interval (milliseconds * 1.6)
        0,   // adv. duration
        0);  // max. num. adv. events

      // Start advertising and enable connections.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);

      // Restart advertising after client has disconnected.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////

      // -------------------------------
      // This event indicates that the value of an attribute in the local GATT
      // database was changed by a remote GATT client.
      case sl_bt_evt_gatt_server_attribute_value_id:
        size_t data_recv_len;

        if ( gattdb_VOLTAGE_START == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_voltageStart;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_VOLTAGE_START, 0, sizeof(data_recv_voltageStart), &data_recv_len, &data_recv_voltageStart);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            vdacOUT_start = (uint16_t)((int16_t)data_recv_voltageStart + vdacOUT_offset_volts);
            calculateLinearSweepStep(); // Recalculate step when voltage range changes
        }

        if ( gattdb_VOLTAGE_STOP == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_voltageStop;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_VOLTAGE_STOP, 0, sizeof(data_recv_voltageStop), &data_recv_len, &data_recv_voltageStop);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            vdacOUT_stop = (uint16_t)((int16_t)data_recv_voltageStop + vdacOUT_offset_volts);
            calculateLinearSweepStep(); // Recalculate step when voltage range changes
        }

        if ( gattdb_VOLTAGE_STEP == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_voltageStep;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_VOLTAGE_STEP, 0, sizeof(data_recv_voltageStep), &data_recv_len, &data_recv_voltageStep);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            if (vdacOUT_stop >= vdacOUT_start) {
                vdacOUT_step = (int16_t) data_recv_voltageStep;
            } else {
                vdacOUT_step = (int16_t) (-1 * data_recv_voltageStep);
            }
        }

        if ( gattdb_PULSE_HEIGHT == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_pulseHeight;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_PULSE_HEIGHT, 0, sizeof(data_recv_pulseHeight), &data_recv_len, &data_recv_pulseHeight);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // Store raw pulse height for pulse mode (always positive)
            pulse_height = data_recv_pulseHeight;
            
            // Keep original logic for square wave voltammetry mode
            if (vdacOUT_stop >= vdacOUT_start) {
                vdacOUT_pulse = -1 * data_recv_pulseHeight;
            } else {
                vdacOUT_pulse = data_recv_pulseHeight;
            }
        }

        if ( gattdb_SAMPLES_PER_PULSE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_samplesPerPulse; // Technically could be 24 bit, but keep at 16 for convenience
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_SAMPLES_PER_PULSE, 0, sizeof(data_recv_samplesPerPulse), &data_recv_len, &data_recv_samplesPerPulse);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            iadcSAMPLESperPULSE = data_recv_samplesPerPulse;
            BLE_packetSize = iadcSAMPLESperPULSE * BLE_DATACHUNKSIZE;
        }

        if ( gattdb_PULSE_WIDTH == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_pulseWidth; // Technically could be 24 bit, but keep at 16 for convenience
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_PULSE_WIDTH, 0, sizeof(data_recv_pulseWidth), &data_recv_len, &data_recv_pulseWidth);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // Store pulse width for pulse mode usage
            pulse_width_ms = data_recv_pulseWidth;
            calculatePulseTiming(); // Recalculate pulse timing when pulse width changes
            
            // Keep original behavior for square wave mode
            uint32_t topValue = (int) ((double) data_recv_pulseWidth * 32.768 / iadcSAMPLESperPULSE);
            LETIMER_TopSet(LETIMER0, topValue);
            LETIMER_CounterSet(LETIMER0, topValue);
        }

        if ( gattdb_TIME_BEFORE_TRIAL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_timeBeforeTrial;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_TIME_BEFORE_TRIAL, 0, sizeof(data_recv_timeBeforeTrial), &data_recv_len, &data_recv_timeBeforeTrial);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            time_before_trial = data_recv_timeBeforeTrial;
        }

        if ( gattdb_TIME_AFTER_TRIAL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_timeAfterTrial;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_TIME_AFTER_TRIAL, 0, sizeof(data_recv_timeAfterTrial), &data_recv_len, &data_recv_timeAfterTrial);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            time_after_trial = data_recv_timeAfterTrial;
        }

        if ( gattdb_GAIN_CHANNEL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_gainChannel;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_GAIN_CHANNEL, 0, sizeof(data_recv_gainChannel), &data_recv_len, &data_recv_gainChannel);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // Ensure the gain channel value is valid (0-3)
            if (data_recv_gainChannel <= 3) {
                gain_channel = data_recv_gainChannel;
            }
        }

        if ( gattdb_ELECTRODE_CHANNEL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_electrodeChannel;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_ELECTRODE_CHANNEL, 0, sizeof(data_recv_electrodeChannel), &data_recv_len, &data_recv_electrodeChannel);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // Ensure the electrode channel value is valid (0-7)
            if (data_recv_electrodeChannel <= 7) {
                electrode_channel = data_recv_electrodeChannel;
            }
        }

        if ( gattdb_OPERATING_MODE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_operatingMode;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_OPERATING_MODE, 0, sizeof(data_recv_operatingMode), &data_recv_len, &data_recv_operatingMode);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // Ensure the operating mode value is valid (0, 1, or 2)
            if (data_recv_operatingMode <= 2) {
                operating_mode = data_recv_operatingMode;
                // Recalculate timing when operating mode changes
                calculateLinearSweepStep();
                calculatePulseTiming();
            }
        }

        if ( gattdb_LINEAR_SWEEP_RATE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_linearSweepRate;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_LINEAR_SWEEP_RATE, 0, sizeof(data_recv_linearSweepRate), &data_recv_len, &data_recv_linearSweepRate);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            linear_sweep_rate = data_recv_linearSweepRate;
            calculateLinearSweepStep(); // Recalculate step when rate changes
        }

        if ( gattdb_LINEAR_SWEEP_SAMPLE_RATE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_linearSweepSampleRate;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_LINEAR_SWEEP_SAMPLE_RATE, 0, sizeof(data_recv_linearSweepSampleRate), &data_recv_len, &data_recv_linearSweepSampleRate);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            linear_sweep_sample_rate = data_recv_linearSweepSampleRate;
            calculateLinearSweepStep(); // Recalculate step when sample rate changes
            calculatePulseTiming(); // Recalculate pulse timing when sample rate changes
        }

        if ( gattdb_TIME_BEFORE_PULSE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_timeBeforePulse;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_TIME_BEFORE_PULSE, 0, sizeof(data_recv_timeBeforePulse), &data_recv_len, &data_recv_timeBeforePulse);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            time_before_pulse = data_recv_timeBeforePulse;
            calculatePulseTiming(); // Recalculate pulse timing when time_before_pulse changes
        }

        if ( gattdb_TIME_AFTER_PULSE == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_timeAfterPulse;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_TIME_AFTER_PULSE, 0, sizeof(data_recv_timeAfterPulse), &data_recv_len, &data_recv_timeAfterPulse);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            time_after_pulse = data_recv_timeAfterPulse;
            calculatePulseTiming(); // Recalculate pulse timing when time_after_pulse changes
        }

        if (gattdb_RUN_EXPERIMENT == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_runExperiment;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_RUN_EXPERIMENT, 0, sizeof(data_recv_runExperiment), &data_recv_len, &data_recv_runExperiment);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            if (data_recv_runExperiment == 0x01) {
              startNewMeasurement();
            } else if (data_recv_runExperiment == 0x0) {
                // Request stop after current pulse completes instead of stopping immediately
                measurement_stop_requested = true;
            }
        }

        break;

      // -------------------------------
      // This event occurs when the remote device enabled or disabled the
      // notification.
      case sl_bt_evt_gatt_server_characteristic_status_id:

        if (gattdb_ADC_RESULT == evt->data.evt_gatt_server_characteristic_status.characteristic) {
          // A local Client Characteristic Configuration descriptor was changed in
          // the gattdb_report_button characteristic.
          if (evt->data.evt_gatt_server_characteristic_status.client_config_flags
              & sl_bt_gatt_notification) {
            // Notification Enabled
//            sc = send_result_notification();
          }
          // } else { // Notification Disabled }
        }
        break;


    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}

/***************************************************************************//**
 * Sends notification of the Report Button characteristic.
 *
 * Reads the current button state from the local GATT database and sends it as a
 * notification.
 ******************************************************************************/


static sl_status_t send_runExperiment_notification()
{
  sl_status_t sc;

  // Send characteristic notification.
  sc = sl_bt_gatt_server_notify_all(gattdb_RUN_EXPERIMENT, sizeof(BLE_value_runExperiment), &BLE_value_runExperiment);
  if (sc != SL_STATUS_OK) { return sc; }

  return sc;
}

