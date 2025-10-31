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



#define RUN_MODE 0 // TREVOR
//#define RUN_MODE 1 // GEORGIA



// IADC Configuration
uint16_t iadcSAMPLESperPULSE = 10; // samples
#define BLE_DATACHUNKSIZE      10
uint16_t BLE_packetSize     = 100; // = iadcSAMPLESperPULSE * BLE_DATACHUNKSIZE
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
static sl_status_t send_result_notification();
volatile bool BLE_notify_runExperiment = false;
volatile bool BLE_notify_result = false;
uint8_t  BLE_value_runExperiment = 0;
uint8_t  BLE_result_0[100]; // Max Size Expected is 10 samplesPerPulse * 10 data chunks = 100
uint8_t  BLE_result_1[100]; // Max Size Expected is 10 samplesPerPulse * 10 data chunks = 100
uint8_t  BLE_result_select  = 1;

// VDAC Configuration
#define VDAC_REF_SELECT vdacRef2V5
#define VDAC_REF_VOLTAGE 2.5
//#define VDAC_REF_SELECT vdacRefAvdd
//#define VDAC_REF_VOLTAGE 3.3


// Application Specific Configuration
#define SWV_REF_VOLTAGE     900 // mV
uint16_t vdacOUT_offset     = 0xFFFF; // VOLTAGE_START * 4096.0 / (double) VDAC_REF_VOLTAGE;
uint16_t vdacOUT_value      = 0xFFFF; // VOLTAGE_START * 4096.0 / (double) VDAC_REF_VOLTAGE;
uint16_t vdacOUT_ref        = ((int) ((double) SWV_REF_VOLTAGE * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
uint32_t vdacOUT_count      = 0;
uint32_t iadcSAMPLE_count   = 0;
bool     iadc_isFirstSample = true;



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

#endif


// Initialization Values Only
#define INITIAL_VOLTAGE_START   900  // mV
#define INITIAL_VOLTAGE_STOP   1300  // mV
#define INITIAL_VOLTAGE_STEP      4  // mV
#define INITIAL_VOLTAGE_PULSE    40  // mV
#define INITIAL_PULSE_WIDTH      50  // ms
uint16_t vdacOUT_start  = ((int) ((double) INITIAL_VOLTAGE_START   * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
uint16_t vdacOUT_stop   = ((int) ((double) INITIAL_VOLTAGE_STOP    * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
 int16_t vdacOUT_step   = ((int) ((double) INITIAL_VOLTAGE_STEP    * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V
 int16_t vdacOUT_pulse  = ((int) ((double) INITIAL_VOLTAGE_PULSE   * 4.096 / (double) VDAC_REF_VOLTAGE)) & 0xFFFF; // 4.096 to divide by 1000 for mv -> V




void startNewMeasurement(void)
{
#if RUN_MODE == 1
  //  GPIO_PinOutClear(C_A0_PORT, C_A0_PIN);
  //  GPIO_PinOutSet(C_A1_PORT, C_A1_PIN);
  //  GPIO_PinOutClear(C_A2_PORT, C_A2_PIN);
  //  GPIO_PinOutSet(EN_1_8_PORT, EN_1_8_PIN);
  //  GPIO_PinOutSet(EN_Vplus_PORT, EN_Vplus_PIN);
  GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
  GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);
#endif

//  sl_sleeptimer_delay_millisecond(5000); // Delay for X milliseconds
  sl_sleeptimer_delay_millisecond(100); // Delay for X milliseconds // TDM MAYBE THIS???

  VDAC_ChannelOutputSet(VDAC_REF_ID, VDAC_REF_CH, vdacOUT_ref);

  if (vdacOUT_offset == 0xFFFF) {
      vdacOUT_offset = vdacOUT_start;

      iadcSAMPLE_count = 0;
      vdacOUT_count  = 0;
      iadc_isFirstSample = true;
      BLE_value_runExperiment = 1;
      BLE_notify_runExperiment = true;
      LETIMER_Enable(LETIMER0, true); // Start the timer

#if RUN_MODE == 0
      GPIO_PinOutClear(LED_OUT_PORT, LED_OUT_PIN);
#endif

  } // else { // Test is already running, do nothing
}

void stopThisMeasurement() {
  LETIMER_Enable(LETIMER0, false);
  vdacOUT_value = vdacOUT_ref;
  VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value);
  BLE_value_runExperiment  = 0;
  BLE_notify_runExperiment = true;
  vdacOUT_offset = 0xFFFF;

#if RUN_MODE == 0
  GPIO_PinOutSet(LED_OUT_PORT, LED_OUT_PIN);
#elif RUN_MODE == 1
  GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
  GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);
  //              GPIO_PinOutSet(LED_OUT_PORT, LED_OUT_PIN);
  //              GPIO_PinOutClear(C_A0_PORT, C_A0_PIN);
  //              GPIO_PinOutSet(C_A1_PORT, C_A1_PIN);
  //              GPIO_PinOutClear(C_A2_PORT, C_A2_PIN);
  //              GPIO_PinOutClear(EN_1_8_PORT, EN_1_8_PIN);
  //              GPIO_PinOutClear(EN_Vplus_PORT, EN_Vplus_PIN);
#endif
}



void IADC_IRQHandler(void)
{
  IADC_Result_t sample;
  uint32_t result_channel0 = 0;
  uint32_t result_channel1 = 0;
  static uint8_t BLE_result_counter = 0;


  if (iadc_isFirstSample) {
      iadc_isFirstSample = false;
//      vdacOUT_value = vdacOUT_offset + vdacOUT_pulse;
//      VDAC_ChannelOutputSet(VDAC_SIG_ID, VDAC_SIG_CH, vdacOUT_value); // TDM MAYBE THIS???
  } else {
    // While the FIFO count is non-zero...
    while (IADC_getScanFifoCnt(IADC0)) {
      // Pull a scan result from the FIFO
      sample = IADC_pullScanFifoResult(IADC0);

      if (sample.id == 0) {
#if RUN_MODE == 0
//          GPIO_PinOutSet(DBG1_OUT_PORT, DBG1_OUT_PIN);
          //double sample_voltage = sample.data * 2.42 / 0xFFF;
#endif
          result_channel0 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
      } else if (sample.id == 1) {
#if RUN_MODE == 0
//          GPIO_PinOutSet(DBG2_OUT_PORT, DBG2_OUT_PIN);
          //double sample_voltage = sample.data * 2.42 / 0xFFF;
#endif
          result_channel1 = ((uint32_t) sample.data) & 0xFFFFF; // 20 bits
      }
    }
    IADC_command(IADC0, iadcCmdStopScan);

    // Construct Packet
    // BLE_result_select points to the packet "ready for BLE to send"
    // therefore BLE_result_select = 1 means 1 is sending and we are filling 0
    uint8_t* BLE_result = BLE_result_select ? BLE_result_0 : BLE_result_1;
    BLE_result[BLE_result_counter+0] = ( result_channel0 & 0x0000FF)      ;
    BLE_result[BLE_result_counter+1] = ( result_channel0 & 0x00FF00) >>  8;
    BLE_result[BLE_result_counter+2] = ( result_channel0 & 0xFF0000) >> 16;
    BLE_result[BLE_result_counter+3] = ( result_channel1 & 0x0000FF)      ;
    BLE_result[BLE_result_counter+4] = ( result_channel1 & 0x00FF00) >>  8;
    BLE_result[BLE_result_counter+5] = ( result_channel1 & 0xFF0000) >> 16;
    BLE_result[BLE_result_counter+6] = (   vdacOUT_value & 0x0000FF)      ;
    BLE_result[BLE_result_counter+7] = (   vdacOUT_value & 0x00FF00) >>  8;
    BLE_result[BLE_result_counter+8] = (iadcSAMPLE_count & 0x0000FF)      ;
    BLE_result[BLE_result_counter+9] = (iadcSAMPLE_count & 0x00FF00) >>  8;

    BLE_result_counter += BLE_DATACHUNKSIZE;
    if (BLE_result_counter >= BLE_packetSize) {
        BLE_result_counter = 0;
        BLE_result_select  = !BLE_result_select;
        BLE_notify_result = true;
    }
  }

#if RUN_MODE == 0
  GPIO_PinOutClear(DBG1_OUT_PORT, DBG1_OUT_PIN);
//  GPIO_PinOutClear(DBG2_OUT_PORT, DBG2_OUT_PIN);
#endif
  IADC_clearInt(IADC0, IADC_IEN_SCANTABLEDONE);
}


void LETIMER0_IRQHandler(void)
{
  uint32_t flags = LETIMER_IntGet(LETIMER0);

  if (flags & 0x1) {
      iadcSAMPLE_count++;
      // Trigger an IADC scan conversion
      IADC_command(IADC0, iadcCmdStartScan);
#if RUN_MODE == 0
      GPIO_PinOutSet(DBG1_OUT_PORT, DBG1_OUT_PIN);
#endif

  } else {
#if RUN_MODE == 0
          GPIO_PinOutSet(DBG2_OUT_PORT, DBG2_OUT_PIN);
#endif
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
          stopThisMeasurement();
        }
      }
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
  VDAC_Init_TypeDef        initRef        = VDAC_INIT_DEFAULT;
  VDAC_InitChannel_TypeDef initChannelSig = VDAC_INITCHANNEL_DEFAULT;
  VDAC_InitChannel_TypeDef initChannelRef = VDAC_INITCHANNEL_DEFAULT;

  // The EM01GRPACLK is chosen as VDAC clock source since the VDAC will be
  // operating in EM1
  // Options: cmuSelect_FSRCO, cmuSelect_HFRCOEM23, cmuSelect_EM01GRPACLK, cmuSelect_EM23GRPACLK
  // If the VDAC is to be operated in EM2 or EM3, VDACn_CLK must be configured to use either HFRCOEM23, EM23GRPACLK or FSRCO instead of the EM01GRPACLK clock.
  // HFRCOEM23 is generally recommended for EM2/EM3 operation
  CMU_ClockSelectSet(cmuClock_VDAC0, cmuSelect_EM01GRPACLK);
  CMU_ClockSelectSet(cmuClock_VDAC1, cmuSelect_EM01GRPACLK);

  // Enable the VDAC clocks
  CMU_ClockEnable(cmuClock_VDAC0, true);
  CMU_ClockEnable(cmuClock_VDAC1, true);
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

  initRef.prescaler    = VDAC_PrescaleCalc(VDAC_REF_ID, (uint32_t) 1000000);
  initRef.reference    = VDAC_REF_SELECT;
  initRef.biasKeepWarm = true; // Set to true if iADC is sharing an internal reference voltage. Costs ~4 uA
  initRef.diff         = false;

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


  // Since the minimum load requirement for high capacitance mode is 25 nF, turn
  // this mode off
  initChannelRef.highCapLoadEnable = false;
  initChannelRef.powerMode = vdacPowerModeHighPower;

  initChannelRef.sampleOffMode = false; // false indicates continuous conversion mode
  initChannelRef.holdOutTime   = 0;     // Set to zero in continuous mode
  initChannelRef.warmupKeepOn  = true;  // Set to true if both channels used to reduce kickback

  initChannelRef.trigMode = vdacTrigModeSw;
  // initChannel.trigMode = vdacTrigModeAsyncPrs; // Therefore triggered by prsConsumerVDAC0_ASYNCTRIGCH0 in LETIMER
  // vdacTrigModeSyncPrs, vdacTrigModeAsyncPrs

  initChannelRef.enable        = true;
  initChannelRef.mainOutEnable = false;
  initChannelRef.auxOutEnable  = true;
  initChannelRef.shortOutput   = false;


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

  initChannelRef.port = VDAC_REF_PORT;
  initChannelRef.pin  = VDAC_REF_PIN;
//  GPIO->ABUSALLOC     = VDAC_REF_BUS;
  GPIO->CDBUSALLOC    = VDAC_REF_BUS;
//  GPIO->BBUSALLOC     = VDAC_REF_BUS;

  VDAC_Init(        VDAC_REF_ID, &initRef);
  VDAC_InitChannel( VDAC_REF_ID, &initChannelRef, VDAC_REF_CH);
  VDAC_Enable(      VDAC_REF_ID, VDAC_REF_CH, true);

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
  IADCconfig_initAllConfigs.configs[0].osrHighAccuracy = iadcCfgOsrHighAccuracy256x; // 5 MHz CLK_ADC --> 0.258 ms per Sample, total = 5 us + 2ch * 258us =  0.521 ms
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
  //NVIC_SetPriority(IADC_IRQn, 1);

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
  //NVIC_SetPriority(LETIMER0_IRQn, 1);

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
  GPIO_PinModeSet(C_A0_PORT, C_A0_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(C_A1_PORT, C_A1_PIN, gpioModePushPull, 0);
  GPIO_PinModeSet(C_A2_PORT, C_A2_PIN, gpioModePushPull, 1);

  GPIO_PinModeSet(EN_1_8_PORT, EN_1_8_PIN, gpioModePushPull, 1);
  GPIO_PinModeSet(EN_Vplus_PORT, EN_Vplus_PIN, gpioModePushPull, 1);

  GPIO_PinModeSet(F_A1_PORT, F_A1_PIN, gpioModePushPull, 1); //00 bottom, 01 top, 10 middle bottom, 11 middle top
  GPIO_PinModeSet(F_A0_PORT, F_A0_PIN, gpioModePushPull, 0);

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
  VDAC_ChannelOutputSet(VDAC_REF_ID, VDAC_REF_CH, vdacOUT_ref);
}

// Application Process Action.
void app_process_action(void)
{
  if (app_is_process_required()) {
  }

  if (BLE_notify_runExperiment) {
      send_runExperiment_notification();
      BLE_notify_runExperiment = false;
  }
  if (BLE_notify_result) {
      send_result_notification();
      BLE_notify_result = false;
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

            vdacOUT_start = data_recv_voltageStart;
        }

        if ( gattdb_VOLTAGE_STOP == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_voltageStop;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_VOLTAGE_STOP, 0, sizeof(data_recv_voltageStop), &data_recv_len, &data_recv_voltageStop);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            vdacOUT_stop = data_recv_voltageStop;
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

            vdacOUT_pulse = data_recv_pulseHeight;

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

            uint32_t topValue = (int) ((double) data_recv_pulseWidth * 32.768 / iadcSAMPLESperPULSE);
            LETIMER_TopSet(LETIMER0, topValue);
            LETIMER_CounterSet(LETIMER0, topValue);
        }

        if ( gattdb_TIME_BEFORE_TRIAL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_timeBeforeTrial;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_PULSE_WIDTH, 0, sizeof(data_recv_timeBeforeTrial), &data_recv_len, &data_recv_timeBeforeTrial);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // TODO Record time before
        }

        if ( gattdb_TIME_AFTER_TRIAL == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint16_t data_recv_timeAfterTrial;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_PULSE_WIDTH, 0, sizeof(data_recv_timeAfterTrial), &data_recv_len, &data_recv_timeAfterTrial);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            // TODO Record Time after
        }

        if (gattdb_RUN_EXPERIMENT == evt->data.evt_gatt_server_attribute_value.attribute) {
            uint8_t data_recv_runExperiment;
            sc = sl_bt_gatt_server_read_attribute_value(gattdb_RUN_EXPERIMENT, 0, sizeof(data_recv_runExperiment), &data_recv_len, &data_recv_runExperiment);
            (void)data_recv_len;
            if (sc != SL_STATUS_OK) { break; }

            if (data_recv_runExperiment == 0x01) {
              startNewMeasurement();
            } else if (data_recv_runExperiment == 0x0) {
                stopThisMeasurement();
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


static sl_status_t send_result_notification()
{
  sl_status_t sc;

  uint8_t* BLE_result = BLE_result_select ? BLE_result_1 : BLE_result_0;
  sc = sl_bt_gatt_server_notify_all(gattdb_ADC_RESULT, BLE_packetSize, BLE_result);
  if (sc != SL_STATUS_OK) { return sc; }

  return sc;
}

