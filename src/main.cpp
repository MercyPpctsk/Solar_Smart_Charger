/*
 * Waveshare ESP32-S3-ETH - Solar charger 4-channel monitor (ADS111x @ 0x48)
 * -------------------------------------------------------------------------
 * ADC is on SDA=GPIO1, SCL=GPIO2. Channel roles (NANOTEC solar charger):
 *   A0 = v_solar      solar-cell voltage      (100k/10k divider, ratio 11)
 *   A1 = v_batt       battery voltage 1S Li-ion (10k/10k divider, ratio 2)
 *   A2 = i_charge     current INTO battery    (ACS712, charging)
 *   A3 = i_discharge  current OUT of battery  (ACS712, load/discharge)
 * NOTE (PCB rev): A1/A2 are SWAPPED vs the older layout. The current-sensor
 *   on the charge path is now wired to A2, the battery divider to A1.
 *
 * ACS712 current sensors:
 *   - Sensitivity = 100 mV/A @ VCC=5V (RATIOMETRIC: scales with VCC)
 *   - Zero-current output = VCC/2 (auto-calibrated; VCC inferred as 2*Vzero)
 *   - Current = (Vout - Vzero) / sensitivity
 *   Small loads (mA) on a 20A sensor sit near the noise floor, so the zero
 *   point MUST be captured with NO current: turn the load OFF and press the
 *   onboard BOOT button (GPIO0) to RE-ZERO, then turn the load ON.
 *   A BOOT-press zero is saved to NVS (Stage 2) and reused on every boot, so
 *   powering up while current flows can no longer fold real current into the
 *   offset (symptom: every reading pinned near 0 mA while current flows).
 *   The one-time boot auto-zero (empty NVS only) is a fallback, never saved.
 *
 * Voltage inputs A0/A1 report pin-voltage * voltGain[] (set the divider ratio
 * there; 1.0 = direct). NEVER feed a pin above (ADS VDD + 0.3V) into the ADC.
 * 1S Li-ion tops out ~4.2V; through the /2 divider that is ~2.1V at A1 - safe.
 *
 * NON-BLOCKING DESIGN (this file):
 *   The original backup (main_adc.cpp.bak) busy-waits through every conversion
 *   (readChAvg blocked loop() for ~1.3 s per report line; zeroing ~5 s). This
 *   version sequences ONE ADS1115 single-shot conversion per state-machine
 *   tick (START -> WAIT -> READ), so loop() stays responsive at all times:
 *   the BOOT button is read every pass, and later stages (NTP/MQTT/sleep) can
 *   hook in without being stalled by the sampler.
 *
 * Timing (ADS1115 @128 SPS single-shot: ~8 ms/conversion, one full
 * START/WAIT/READ cycle ~= 11 ms incl. I2C overhead):
 *   report round: 4 ch x 128 samples = 512 conv.  => one line every ~5.6 s,
 *                 every channel sampled evenly across the SAME time window
 *   zero round  : 2 ch x 256 samples = 512 conv.  => ~5.6 s, non-blocking
 *
 * Onboard WS2812 RGB LED (GPIO21) status:
 *   BLUE = zeroing     GREEN = reading OK     RED = ADC not responding
 *
 * STAGE 4 - NTP clock + periodic CSV logging (all non-blocking):
 *   WiFi (credentials in config.h) brings up SNTP; configTzTime("UTC-7")
 *   keeps local time at ICT (UTC+7). Until the first sync, timestamps fall
 *   back to "boot+Ns". Every report round feeds an aggregator; once a minute
 *   one CSV line (means of that minute) is appended to a daily file on the
 *   LittleFS partition (~3.4 MB); only the newest 30 daily files are kept.
 *   Serial commands: 't' clock/WiFi status, 'd' dump tail of the current
 *   log, 'E' erase all log files.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <stdlib.h>     // qsort (used by the trimmed-mean spike rejection)
#include <string.h>     // strcmp (used by deriveSolStatus full-battery exception)
#include "config.h"     // MUST come before the #if ENABLE_NETWORK block below:
                        // config.h defines ENABLE_NETWORK, which gates WiFi/NTP/
                        // LittleFS/IQAir/SoC includes. (Undefined in #if == 0.)
#if ENABLE_NETWORK
#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "iqair_client.h"   // sync fetch() runs on a pinned core-0 task (non-blocking to loop)
#include <PubSubClient.h>   // ThingsBoard MQTT client (runs on core 0)
#include "setup_web.h"      // SoftAP + Web configuration portal
#endif

#define RGB_PIN     21
#define BOOT_BTN    0     // onboard BOOT button, active LOW
#define I2C_SDA     1
#define I2C_SCL     2
#define ADS_ADDR    0x48

// ADS111x / ADS101x register pointers
#define REG_CONVERSION  0x00
#define REG_CONFIG      0x01

// PGA = +/-6.144V (gain = 2/3). Full-scale range used for the voltage math.
static const float FS_VOLTS = 6.144f;

// Base config: OS=1 (start), PGA=000 (+/-6.144V), MODE=1 (single-shot),
// DR=100 (128SPS on ADS1115), comparator disabled (COMP_QUE=11).
// MUX bits [14:12] are OR-ed in per channel below.
static const uint16_t CONFIG_BASE = 0x8183;

// Conversion time budget: 128 SPS => ~7.8 ms per conversion; WAIT gives it
// 10 ms of settle so the READ poll normally sees OS=1 immediately.
static const uint16_t CONV_WAIT_MS = 10;

// Samples per channel. ALL channels in a report round use the SAME count so
// they fill at the same rate and are averaged over the SAME time window.
// (Mixed counts let the smaller channels finish early and get skipped, which
// desynchronised the averages when signals drifted mid-round.)
// Round = 4 ch x 128 = 512 conversions ~= 5.6 s per report line. The current
// channels then take a 10%-trimmed mean (see ChanAcc::computeTrimmed) which
// rejects one-off load-switching spikes on A3 without slowing the round down.
static const uint16_t CH_SAMPLES    = 128;
static const uint16_t ZERO_SAMPLES  = 256;
// Current channels keep every raw sample so a trimmed mean can drop the worst
// outliers (spikes) before averaging. Sized to CH_SAMPLES (zero mode uses the
// plain mean() and never trims, so it doesn't need this buffer).
static const uint16_t CUR_BUF_SAMPLES = CH_SAMPLES;
// % of samples dropped from EACH tail before averaging the current channels.
// 10% trims the worst ~13 of 128 samples top & bottom - removes one-off load
// switching spikes on A3 without distorting a steady current.
static const uint8_t  CUR_TRIM_PCT  = 10;

// ---- Channel roles -------------------------------------------------------
// A2, A3 = ACS712 current sensors ; A0, A1 = voltage inputs.
//   A2 = i_charge (into battery) ; A3 = i_discharge (out of battery/load)
// (PCB rev: A1/A2 swapped vs the older layout - charge current is on A2,
//  battery divider on A1. CUR_CH/VOLT_CH/voltGain reflect this wiring.)
static const uint8_t CUR_CH[2]  = {2, 3};   // current-sensor channels
static const uint8_t VOLT_CH[2] = {0, 1};   // voltage-measurement channels

// ACS712-20A: 100 mV/A at the nominal 5V supply. Ratiometric, so we scale it
// by the measured VCC (VCC = 2 * zero-current output) during calibration.
static const float ACS712_V_PER_A_AT_5V = 0.100f;
static const float ACS712_VCC_NOMINAL   = 5.0f;

// Voltage-input scaling: report Vin = Vadc * ratio, where ratio is the
// resistor-divider factor (R_top+R_bot)/R_bot. Index by ADC channel 0..3.
//   A0 = v_solar via 100k(top)/10k(bottom) divider -> ratio 11.0 (~1.6V @ 18V)
//   A1 = v_batt (1S Li-ion) via 10k/10k divider    -> ratio 2.0  (2.1V @ 4.2V)
// WIRING: R_top in series from the source (+), R_bot to GND. NEVER swap them.
static const float VDIV_RATIO_SOLAR = (100000.0f + 10000.0f) / 10000.0f;  // = 11.0
static const float VDIV_RATIO_BATT  = ( 10000.0f + 10000.0f) / 10000.0f;  // = 2.0
static float voltGain[4] = {VDIV_RATIO_SOLAR, VDIV_RATIO_BATT, 1.0f, 1.0f};
//                           A0=/11 solar      A1=/2 batt         A2   A3

// Auto-calibrated (assumes no current at cal time). Indexed by ADC channel:
static float vZero[4] = {2.5f, 2.5f, 2.5f, 2.5f};  // per-channel zero voltage
static float vPerA    = 0.100f;                    // ACS712 sensitivity (V/A)

// Direction sign: bench wiring drives Vout BELOW the zero point for current
// flowing in the labelled positive direction (A2 = INTO battery, A3 = OUT to
// load), so raw readings come out NEGATIVE while charging/discharging. Flip
// them so +Chg = charging and +Dis = load current. With the current wiring
// (verified 2026-09-15: raw charge ch rises ABOVE its zero point while
// charging), the physical polarity is already correct, so NO flip is applied
// for Chg. Set to -1.0f only if the sensors are ever physically re-oriented.
// Direction sign:
//   A2 (Charge into battery): Vout rises ABOVE zero point -> +1.0f
//   A3 (Discharge to load):   Vout drops BELOW zero point -> -1.0f (flip so +Dis = positive load)
static const float CUR_SIGN_FLIP_CHG =  1.0f;
static const float CUR_SIGN_FLIP_DIS = -1.0f;

// ---- Stage 2: NVS-persisted zero calibration ------------------------------
// A zero captured via the BOOT button is stored in NVS and reused across
// reboots. Boot auto-zero (empty NVS) stays a fallback and is never stored,
// so a power-up with current flowing can no longer fold real current into
// the offset (that pinned all readings near 0 mA while current flowed).
static Preferences prefs;
static const char *PREFS_NS  = "solarcal";
// NVS keys are named by CURRENT ADC channel, so a PCB channel swap cannot
// reuse a zero stored against the OLD channel role. A2/A3 are the ACS712
// current channels on the current PCB rev.
static const char *KEY_VZ2   = "vzero2";
static const char *KEY_VZ3   = "vzero3";
static const char *KEY_VPA   = "vperA";
static const char *KEY_VPA_CAL       = "vperAcal";   // field-cal sensitivity (overrides KEY_VPA)
static const char *KEY_VPA_CAL_VALID = "vpaCalOk";
static const char *KEY_VALID = "valid";
static bool zeroSavePending  = false;   // set by BOOT press, consumed in finishZero()

// Most-recent raw chg(A2)/dis(A3) volts (updated every report round). Used by
// the 'C' field-calibration command: sens = (raw - zero) / measuredAmps.
static float lastReportChg = 0.0f, lastReportDis = 0.0f;

#if ENABLE_NETWORK
// ---- Stage 4a: NTP clock over WiFi (background, polled - never blocks) -----
static const char *TZ_STRING      = "UTC-7";        // POSIX: UTC-7 == UTC+7 (ICT)
static const char *NTP_SERVER_1   = "pool.ntp.org";
static const char *NTP_SERVER_2   = "time.google.com";
static const time_t NTP_MIN_EPOCH = 1750000000;     // ~2025-06: later == synced
static bool ntpSynced = false;

// ---- Stage 4b: periodic CSV logging to LittleFS ----------------------------
// Each report round feeds an aggregator; once a minute ONE mean line is
// appended to a daily file (before NTP sync: /log_boot.csv). ~90 B/min.
static const uint32_t LOG_INTERVAL_MS = 60000UL;    // one CSV line per minute
static const uint8_t  LOG_KEEP_FILES  = 30;         // newest daily files kept

struct LogAgg {                        // means over one minute of report rounds
  uint32_t n = 0;
  double   vSolar = 0, vBatt = 0, iChg = 0, iDis = 0;
  void add(float vs, float vb, float ic, float id) {
    vSolar += vs; vBatt += vb; iChg += ic; iDis += id; ++n;
  }
  void reset() { n = 0; vSolar = vBatt = iChg = iDis = 0; }
};
static LogAgg   logAgg;
static uint32_t lastLogMs = 0;         // first line lands ~60 s after boot
static bool     fsOk      = false;

// ---- Stage 5: IQAir non-blocking fetch on a pinned core-0 task ----------------
// iqair_client.h::fetch() is synchronous (TLS handshake + HTTP GET ~1-2 s).
// On ESP32-S3, Arduino loop()/setup() run on core 1 (ARDUINO_RUNNING_CORE=1),
// so the ADC sampler lives on core 1. The WiFi/esp_wifi stack runs on core 0.
// We pin the fetch task to CORE 0 (same core as WiFi) so the blocking fetch()
// NEVER touches core 1 - the ADC sampler keeps its 11 ms cadence uninterrupted.
// iqair_client.h stays untouched; the task just hosts its sync fetch() on a
// core that the sampler does not use.
static IQAirClient  iqair(IQAIR_STATION_URL);
static IQAirData    iqairData;                 // shared: written by task, read by loop (mutex)
static SemaphoreHandle_t iqairMutex = nullptr; // guards iqairData snapshot copy
static TaskHandle_t iqairTaskHandle = nullptr;
static const unsigned long IQAIR_INTERVAL_MS = 30UL * 60UL * 1000UL;  // 30 min
static const uint32_t IQAIR_TASK_STACK = 16 * 1024;   // TLS needs ~8-12 KB
static const BaseType_t IQAIR_TASK_CORE = 0;          // WiFi core; NOT the ADC core

// ---- Stage 6: SoC by coulomb counting (LiFePO4 1S 150 Ah) --------------------
// LiFePO4 has a nearly FLAT voltage curve across ~20-90% SoC, so voltage-based
// SoC is useless. We integrate net current (Chg - Dis) over time instead, with
// automatic endpoint recalibration at the two voltage points where LiFePO4 is
// actually unambiguous: ~3.65 V = full (100%), ~2.50 V = empty (0%).
//   SoC_delta = (Net_A * dt_hours / Capacity_Ah) * 100 [%]
// The running SoC is persisted to NVS every 5 min so it survives reboots; the
// 'S<%>' serial command lets the operator set a known SoC manually.
static const float BAT_CAP_AH = 150.0f;   // battery capacity (Ah)
static const float SOC_HI_V   = 3.65f;    // full-charge voltage -> 100%
static const float SOC_LO_V   = 2.50f;    // empty voltage       -> 0%
static const char  *KEY_SOC   = "socPct"; // NVS key for SoC
static float       socPct     = 50.0f;    // running SoC [%]; loaded from NVS in setup()
static uint32_t    socLastMs  = 0;         // last integration timestamp
static uint32_t    socSaveMs  = 0;         // last NVS save timestamp
static const uint32_t SOC_SAVE_MS = 5UL * 60UL * 1000UL;  // persist every 5 min

// ---- Stage 7: derived telemetry snapshot for the (future) MQTT stage --------
// finishReport() computes everything once per round (~5.6 s) and writes it into
// a single struct guarded by a FreeRTOS mutex. The MQTT task (core 0) reads it
// through ONE function - getValue() - and never touches ADC/SoC/IQAir logic.
//
// Producer = finishReport() on core 1 (holds the mutex for microseconds only:
// plain struct assignment, no I/O inside the lock). Reader = getValue() on core
// 0 uses a BOUNDED 10 ms timeout so it can never block the WiFi/MQTT stack even
// in the pathological case; on timeout it returns a zero snapshot the caller
// can detect (uptime_s == 0). The two mutexes (iqairMutex, telemetryMutex) are
// NEVER nested - iqairSnapshot() is called and fully released BEFORE the
// telemetry lock is taken, so there is no deadlock window.

// Boot attributes - sent once as shared attributes at MQTT connect time.
// PLACEHOLDER values: confirm before field deploy.
static const char  *FW_VERSION       = "0.4.0-pre-mqtt";  // TODO: bump on release
static const char  *DEVICE_ID        = "solar-01";        // TODO: 01-04 per unit
static const float  BATT_CAPACITY_AH = BAT_CAP_AH;        // single source of truth

// Alert thresholds. SOC low is TOR-confirmed (3.2.1: <20% -> low_battery).
// The two voltage thresholds are PLACEHOLDERS that assume the real 4S pack:
//   ALERT_VBATT_FAULT_V must match the pack BMS low-voltage cutoff (not 1S).
//   ALERT_VSOLAR_OV_V   was left open in the TOR - confirm against panel Voc.
// Both are SKIPPED while BENCH_1S == 1 (see config.h) so a 3.3V bench cell does
// not permanently trip "fault".
static const float ALERT_SOC_LOW_PCT   = 20.0f;   // TOR 3.2.1: soc < 20% -> low_battery
static const float ALERT_VBATT_FAULT_V = 10.5f;   // PLACEHOLDER - 4S BMS cutoff
static const float ALERT_VSOLAR_OV_V   = 24.0f;   // PLACEHOLDER - panel Voc max
static const float SOLAR_DAY_THRESHOLD_V = 1.0f;  // vSolar above this = panel sees light

struct TelemetryValues {
  // TOR 3.2.2 Voltage Monitor (real-time graph)
  float v_solar = 0, v_batt = 0;
  // TOR 3.2.4 Battery Condition (gauge): current in/out + power
  float i_solar = 0, p_solar = 0;   // i_solar = charge INTO battery
  float i_load  = 0, p_load  = 0;   // i_load  = discharge OUT to load
  float soc     = 0;                // State of Charge [%]
  // TOR 3.2.5 Battery Health (gauge)
  float soh     = 100.0f;           // State of Health [%] via Equivalent Full Cycles
  // TOR 3.2.6 energy produce/consume statistics (reset at local midnight)
  float energy_in_wh  = 0, energy_out_wh = 0;
  // TOR 3.2.1 notifications (split into 3 subsystems) + 3.2.6 system overview
  const char *bat_status = "idle";   // full|charging|discharging|low_battery|fault|idle
  const char *sol_status = "true";   // true|fault
  const char *iq_status  = "offline";// enable|fetch_fail|offline
  uint32_t    uptime_s = 0;
  // Boot attributes (constant; MQTT stage sends once as shared attributes)
  const char *fw_version    = FW_VERSION;
  const char *device_id     = DEVICE_ID;
  float       batt_capacity = BATT_CAPACITY_AH;
  // TOR 3.2.6 IQAir air-quality (7 fields, merged so getValue() returns all)
  bool  iq_valid = false;
  int   aqi_us   = 0;
  float pm1 = 0, pm25 = 0, pm10 = 0;
  float temp_c = 0, humidity = 0, pressure = 0;   // degC, %RH, mbar
  String iq_ts;                                    // IQAir sample timestamp
};
static TelemetryValues   telemetry;               // written by finishReport (core 1)
static SemaphoreHandle_t telemetryMutex = nullptr; // guards the snapshot copy

// ---- energy_in / energy_out (Wh), reset at local midnight ------------------
static double  energyInWh  = 0.0;
static double  energyOutWh = 0.0;
static int     energyDay   = -1;     // day-of-year of last reset; -1 = never yet
static uint32_t nrgLastMs  = 0;      // own timer, independent of serviceSoC()'s

static void serviceEnergy(float pSolar, float pLoad) {
  uint32_t now = millis();
  if (nrgLastMs == 0) { nrgLastMs = now; return; }   // first call: just stamp
  float dt_h = (float)(now - nrgLastMs) / 3600000.0f;
  nrgLastMs = now;
  energyInWh  += (double)pSolar * dt_h;
  energyOutWh += (double)pLoad  * dt_h;
  if (ntpSynced) {
    time_t t = time(nullptr);
    struct tm tv; localtime_r(&t, &tv);
    if (tv.tm_yday != energyDay) {          // date rolled over (or first sync)
      if (energyDay != -1) {                // skip the "reset" message on first sync
        Serial.printf("[NRG ] midnight rollover: in=%.2f Wh out=%.2f Wh -> reset\n",
                      energyInWh, energyOutWh);
        energyInWh = 0.0;
        energyOutWh = 0.0;
      }
      energyDay = tv.tm_yday;
    }
  }
}

// ---- State of Health via Equivalent Full Cycles (EFC) ----------------------
// LiFePO4 ages primarily by charge throughput, so we accumulate |Ah| (charge +
// discharge) and map full cycles onto a linear capacity-fade curve. This is the
// method mid-range commercial BMS units use: not a direct capacity measurement,
// but a sound, monotonic health indicator for a dashboard gauge (TOR 3.2.5).
//   EFC = ahThroughput / (2 * BAT_CAP_AH)   (one full cycle = charge+discharge)
//   SoH = 100 - (EFC / CYCLE_LIFE) * (100 - END_OF_LIFE_SOH)
static double  ahThroughput = 0.0;          // |charge|+|discharge| Ah, NVS-persisted
static const char *KEY_AH_TP = "ahTp";      // NVS key for throughput
static uint32_t ahLastMs  = 0;              // own timer (decoupled from SoC)
static uint32_t sohSaveMs = 0;
static const uint32_t SOH_SAVE_MS = 60UL * 60UL * 1000UL;  // persist hourly
static float sohPct = 100.0f;
// TODO: confirm from the real cell's spec sheet. 2000 cycles @80% DoD is a
// typical LiFePO4 mid-grade value; premium cells reach 4000-6000.
static const float CYCLE_LIFE      = 2000.0f;
static const float END_OF_LIFE_SOH = 80.0f;   // LiFePO4 EOL convention (80% cap)

static void serviceSoH(float chgA, float disA) {
  uint32_t now = millis();
  if (ahLastMs == 0) { ahLastMs = now; return; }   // first call: just stamp
  float dt_h = (float)(now - ahLastMs) / 3600000.0f;
  ahLastMs = now;
  ahThroughput += (fabsf(chgA) + fabsf(disA)) * dt_h;
  float efc = (float)ahThroughput / (2.0f * BAT_CAP_AH);
  sohPct = 100.0f - (efc / CYCLE_LIFE) * (100.0f - END_OF_LIFE_SOH);
  if (sohPct < END_OF_LIFE_SOH) sohPct = END_OF_LIFE_SOH;
  if (sohPct > 100.0f)          sohPct = 100.0f;
  if (now - sohSaveMs > SOH_SAVE_MS || sohSaveMs == 0) {
    sohSaveMs = now;
    prefs.putDouble(KEY_AH_TP, ahThroughput);
  }
}

// ---- per-subsystem status derivation ---------------------------------------
// Pure functions of this round's readings - call AFTER serviceSoC() so soc
// already reflects this round. BENCH_1S skips the voltage-threshold branches
// (their placeholders target a 12V pack and would always trip on a 1S bench).
//
// Battery priority (highest -> lowest):
//   1. fault        vBatt <= BMS cutoff (4S guard)          -- BENCH_1S skips
//   2. low_battery  soc < 20%  (covers "device off while not charging")
//   3. full         soc >= 99.5% && charging into pack
//   4. charging     net current into battery
//   5. discharging  net current out to load
//   6. idle         otherwise
//
// Solar fault = panel voltage over Voc  OR  panel sees light but produces no
// current while the battery is NOT full (the full-battery case is normal: the
// charger holds off, so current is ~0 in daylight and must NOT be flagged).
//
// IQAir status mirrors connectivity + last successful fetch:
//   offline     WiFi down
//   fetch_fail  WiFi up but no valid sample yet (server/API error)
//   enable      WiFi up and a valid sample is cached
static const char *deriveBatStatus(float chgA, float disA, float vBatt, float soc) {
  static const float I_IDLE_A = 0.05f;   // below this, channel = "no current"
#if !BENCH_1S
  if (vBatt <= ALERT_VBATT_FAULT_V)         return "fault";        // priority 1
#endif
  if (soc < ALERT_SOC_LOW_PCT)              return "low_battery";  // priority 2
  if (soc >= 99.5f && chgA > I_IDLE_A)      return "full";         // priority 3
  if (chgA > disA + I_IDLE_A)               return "charging";     // priority 4
  if (disA > chgA + I_IDLE_A)               return "discharging";  // priority 5
  return "idle";                                                     // priority 6
}

static const char *deriveSolStatus(float vSolar, float chgA, const char *batStatus) {
  static const float I_IDLE_A = 0.05f;
#if !BENCH_1S
  if (vSolar > ALERT_VSOLAR_OV_V)           return "fault";        // over Voc
  // Panel sees light but produces (near) no current, and the battery is not
  // full -> a healthy panel in daylight should be delivering current.
  if (vSolar > SOLAR_DAY_THRESHOLD_V && chgA <= I_IDLE_A &&
      strcmp(batStatus, "full") != 0)       return "fault";
#endif
  return "true";
}

static const char *deriveIqStatus(bool wifiUp, bool iqValid) {
  if (!wifiUp)                              return "offline";
  return iqValid ? "enable" : "fetch_fail";
}

// The single read entry point for the MQTT stage (and any other consumer).
// Returns a private, stable copy of the latest telemetry snapshot. Bounded
// 10 ms timeout: if the producer is somehow still holding the lock, the caller
// is NOT blocked indefinitely - it gets a zero snapshot (uptime_s == 0) it can
// detect and skip/drop. In practice the producer holds the mutex for only
// microseconds (plain struct copy, no I/O in the lock), so this path almost
// always succeeds on the first try.
static TelemetryValues getValue() {
  TelemetryValues out;
  if (!telemetryMutex) return out;                 // before setup() finishes
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    out = telemetry;                               // struct copy incl. String (deep)
    xSemaphoreGive(telemetryMutex);
  }
  return out;                                      // RVO; caller owns the copy
}

static void wifiNtpBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  // Print the disconnect REASON once per attempt burst: 201 = AP not found
  // (out of range / 5 GHz only), 15 = wrong password, 2 = auth timeout.
  static bool reasonHookInstalled = false;
  if (!reasonHookInstalled) {
    reasonHookInstalled = true;
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
      static uint32_t lastPrint = 0;
      if (millis() - lastPrint < 5000) return;    // rate-limit retries
      lastPrint = millis();
      Serial.printf("[NET ] disconnect reason=%d (201 no AP, 15 bad password, 2 auth timeout)\n",
                    info.wifi_sta_disconnected.reason);
    }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  if (appConfig.wifi_ssid[0] != '\0') {
    WiFi.begin(appConfig.wifi_ssid, appConfig.wifi_pass);
    Serial.printf("[NET ] WiFi connecting to \"%s\"; NTP %s,%s (TZ %s)\n",
                  appConfig.wifi_ssid, NTP_SERVER_1, NTP_SERVER_2, TZ_STRING);
  } else {
    Serial.println("[NET ] No WiFi SSID configured yet. Connect to SoftAP to configure.");
  }
  configTzTime(TZ_STRING, NTP_SERVER_1, NTP_SERVER_2);
}

// Polled at 1 Hz max; prints state CHANGES only, never blocks loop().
static void serviceNetwork() {
  if (g_wifiChanged) {
    g_wifiChanged = false;
    Serial.printf("[NET ] Applying new WiFi SSID: \"%s\"...\n", appConfig.wifi_ssid);
    WiFi.disconnect();
    if (appConfig.wifi_ssid[0] != '\0') {
      WiFi.begin(appConfig.wifi_ssid, appConfig.wifi_pass);
    }
  }

  static uint32_t lastMs = 0;
  static bool     wifiUp = false;
  uint32_t now = millis();
  if (now - lastMs < 1000) return;
  lastMs = now;

  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != wifiUp) {
    wifiUp = up;
    if (up) Serial.printf("[NET ] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
    else    Serial.println("[NET ] WiFi lost, auto-reconnecting...");
  }
  if (!up || ntpSynced) return;
  time_t t = time(nullptr);
  if (t > NTP_MIN_EPOCH) {
    ntpSynced = true;
    struct tm tv; localtime_r(&t, &tv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tv);
    Serial.printf("[TIME] NTP synced: %s\n", buf);
  }
}

// ---- Stage 5: IQAir fetch task (core 0) + control helpers -------------------
// Runs on core 0 (the WiFi core): waits for WiFi, then loops fetch() -> snapshot
// copy. The synchronous fetch() blocks HERE (core 0) but never on the ADC's
static bool g_newIqairReady = false;

// core 1. 'i' / setSoc notify it via xTaskNotifyGive for an immediate fetch.
static void iqairTask(void *arg) {
  Serial.println("[IQAir] task started (core 0), waiting for WiFi...");
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(2000));
  Serial.println("[IQAir] WiFi up, starting fetch loop");

  IQAirData local;          // scratch buffer: fetch() prints its own [IQAir] line
  bool first = true;
  for (;;) {
    // Sleep dynamic interval (default 30 min), unless notified (immediate fetch via 'i').
    uint32_t intervalMs = (appConfig.iqair_interval > 0 ? appConfig.iqair_interval : 30) * 60000UL;
    if (!first) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(intervalMs));
    first = false;

    // If WiFi dropped during the wait, hold until it is back so we never
    // hammer a dead link (polls every 5 s, yields the core while waiting).
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[IQAir] WiFi lost, pausing fetch loop");
      while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(5000));
      Serial.println("[IQAir] WiFi back, resuming fetch loop");
    }

    if (g_iqairChanged) {
      g_iqairChanged = false;
      iqair.setUrl(appConfig.iqair_url);
      Serial.printf("[IQAir] URL updated to '%s' (interval: %u min)\n",
                    appConfig.iqair_url, appConfig.iqair_interval);
    }

    // fetch() is synchronous (TLS + HTTP GET ~1-2 s) but we are on core 0
    // (the WiFi core): the ADC sampler on core 1 keeps running uninterrupted.
    if (iqair.fetch(local)) {
      // Publish the fresh result to the shared buffer under the mutex. This
      // copy is a few dozen bytes and takes microseconds - loop() on core 1
      // can read it any time without blocking.
      xSemaphoreTake(iqairMutex, portMAX_DELAY);
      iqairData = local;
      xSemaphoreGive(iqairMutex);
      g_newIqairReady = true;  // Flag for ThingsBoard to publish separate IQAir telemetry
    }
  }
}

// Reads a thread-safe SNAPSHOT of the latest IQAir data (loop side, core 1).
// Returns false if no successful fetch yet. Never blocks on the fetch itself.
static bool iqairSnapshot(IQAirData &out) {
  if (!iqairMutex) return false;
  xSemaphoreTake(iqairMutex, portMAX_DELAY);
  out = iqairData;
  xSemaphoreGive(iqairMutex);
  return out.valid;
}

// Wake the fetch task now (used by the 'i' serial command). Instant: it just
// posts a notification and returns; the actual fetch happens on core 0.
static void iqairRequestFetch() {
  if (iqairTaskHandle) xTaskNotifyGive(iqairTaskHandle);
}

static void iqairBegin() {
  if (appConfig.iqair_url[0] != '\0') {
    iqair.setUrl(appConfig.iqair_url);
  }
  iqairMutex = xSemaphoreCreateMutex();
  // Pin to core 0 (IQAIR_TASK_CORE): loop()/ADC run on core 1, WiFi on core 0.
  // Putting the blocking fetch on the WiFi core keeps the ADC sampler on core 1
  // fully non-blocking - the two never contend for the same CPU.
  xTaskCreatePinnedToCore(iqairTask, "iqair", IQAIR_TASK_STACK, nullptr,
                         1, &iqairTaskHandle, IQAIR_TASK_CORE);
}

// ---- Stage 8: ThingsBoard MQTT integration (pinned to Core 0) --------------
// Runs on core 0 (the WiFi/network core). Never touches ADC sampler on core 1.
// Reads telemetry snapshot via getValue() with 10 ms bounded mutex lock.
static WiFiClient       tbWifiClient;
static PubSubClient     tbMqtt(tbWifiClient);
static TaskHandle_t     tbTaskHandle = nullptr;
static const uint32_t   TB_TASK_STACK = 8192;
static const BaseType_t TB_TASK_CORE  = 0;          // WiFi core; NOT the ADC core

static void tbPublishAttributes(const TelemetryValues &tv) {
  JsonDocument doc;
  doc["fw_version"]    = tv.fw_version;
  doc["device_id"]     = (appConfig.dev_name[0] != '\0') ? appConfig.dev_name : tv.device_id;
  doc["batt_capacity"] = tv.batt_capacity;

  char buf[256];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0) {
    bool ok = tbMqtt.publish(TB_MQTT_TOPIC_ATTRIBUTES, buf);
    Serial.printf("[TB  ] Attributes sent (%u B): %s [%s]\n",
                  (unsigned)len, buf, ok ? "OK" : "FAILED");
  }
}

static void tbPublishIQAir(const IQAirData &iq) {
  if (!iq.valid) return;
  JsonDocument doc;
  doc["aqi"]      = iq.aqi_us;
  doc["pm1"]      = serialized(String(iq.pm1, 1));
  doc["pm25"]     = serialized(String(iq.pm25, 1));
  doc["pm10"]     = serialized(String(iq.pm10, 1));
  doc["temp_air"] = serialized(String(iq.temp_c, 1));
  doc["humidity"] = serialized(String(iq.humidity, 0));
  doc["pressure"] = serialized(String(iq.pressure, 1));

  char buf[256];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0) {
    const char* topic = (appConfig.tb_topic[0] != '\0') ? appConfig.tb_topic : TB_MQTT_TOPIC_TELEMETRY;
    bool ok = tbMqtt.publish(topic, buf);
    Serial.printf("[TB  ] IQAir Telemetry sent (%u B) [%s]\n", (unsigned)len, ok ? "OK" : "FAILED");
    if (!ok) {
      Serial.printf("[TB  ] IQAir publish failed, payload: %s\n", buf);
    }
  }
}

static void tbPublishTelemetry(const TelemetryValues &tv) {
  JsonDocument doc;
  // 3.2.2 Voltage Monitor
  doc["v_solar"]        = serialized(String(tv.v_solar, 2));
  doc["v_batt"]         = serialized(String(tv.v_batt, 2));
  // 3.2.4 Battery Condition
  doc["i_solar"]        = serialized(String(tv.i_solar, 3));
  doc["p_solar"]        = serialized(String(tv.p_solar, 1));
  doc["i_load"]         = serialized(String(tv.i_load, 3));
  doc["p_load"]         = serialized(String(tv.p_load, 1));
  doc["soc"]            = serialized(String(tv.soc, 1));
  // 3.2.5 Battery Health
  doc["soh"]            = serialized(String(tv.soh, 1));
  // 3.2.6 Energy stats
  doc["energy_in"]      = serialized(String(tv.energy_in_wh, 2));
  doc["energy_out"]     = serialized(String(tv.energy_out_wh, 2));
  // TOR 3.2.1 Subsystem statuses (V02 NEW KEYS!)
  doc["bat_status"]     = tv.bat_status;
  doc["sol_status"]     = tv.sol_status;
  doc["iq_status"]      = tv.iq_status;
  doc["uptime"]         = tv.uptime_s;

  char buf[512];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  if (len > 0) {
    const char* topic = (appConfig.tb_topic[0] != '\0') ? appConfig.tb_topic : TB_MQTT_TOPIC_TELEMETRY;
    bool ok = tbMqtt.publish(topic, buf);
    Serial.printf("[TB  ] Telemetry sent (%u B) [%s]\n", (unsigned)len, ok ? "OK" : "FAILED");
    if (!ok) {
      Serial.printf("[TB  ] Publish failed, payload: %s\n", buf);
    }
  }
}

static void tbMqttTask(void *arg) {
  Serial.println("[TB  ] task started (core 0), waiting for WiFi...");
  tbMqtt.setServer(appConfig.tb_host, appConfig.tb_port);
  tbMqtt.setBufferSize(1024);

  uint32_t lastPubMs = 0;
  uint32_t lastConnectAttemptMs = 0;
  bool attrSent = false;
  bool initialIqairSent = false;

  for (;;) {
    setupWebTick();   // Run Web UI + Captive Portal DNS on Core 0

    if (g_tbChanged) {
      g_tbChanged = false;
      Serial.printf("[TB  ] Cloud config updated -> reconnecting to %s:%u...\n",
                    appConfig.tb_host, appConfig.tb_port);
      tbMqtt.disconnect();
      tbMqtt.setServer(appConfig.tb_host, appConfig.tb_port);
      attrSent = false;
      initialIqairSent = false;
    }

    if (WiFi.status() != WL_CONNECTED) {
      attrSent = false;
      initialIqairSent = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t now = millis();

    if (!tbMqtt.connected()) {
      attrSent = false;
      initialIqairSent = false;
      if (now - lastConnectAttemptMs >= 5000 || lastConnectAttemptMs == 0) {
        lastConnectAttemptMs = now;
        Serial.printf("[TB  ] Connecting to %s:%d...\n", appConfig.tb_host, appConfig.tb_port);
        const char* activeDevId = (appConfig.dev_name[0] != '\0') ? appConfig.dev_name : DEVICE_ID;
        if (tbMqtt.connect(activeDevId, appConfig.tb_token, nullptr)) {
          Serial.println("[TB  ] Connected to ThingsBoard!");
          TelemetryValues tv = getValue();
          if (tv.uptime_s > 0) {
            tbPublishAttributes(tv);
            attrSent = true;
          }
        } else {
          Serial.printf("[TB  ] Connection failed, rc=%d (retry in 5s)\n", tbMqtt.state());
        }
      }
    } else {
      tbMqtt.loop();

      if (!attrSent) {
        TelemetryValues tv = getValue();
        if (tv.uptime_s > 0) {
          tbPublishAttributes(tv);
          attrSent = true;
        }
      }

      // If new IQAir data was just fetched, or first connect with valid data -> send separate IQAir telemetry
      if (g_newIqairReady || !initialIqairSent) {
        IQAirData iq;
        if (iqairSnapshot(iq) && iq.valid) {
          g_newIqairReady = false;
          initialIqairSent = true;
          tbPublishIQAir(iq);
        }
      }

      uint32_t intervalMs = (appConfig.tb_interval > 0 ? appConfig.tb_interval : 15) * 1000UL;
      if (now - lastPubMs >= intervalMs || lastPubMs == 0) {
        TelemetryValues tv = getValue();
        if (tv.uptime_s > 0) {
          lastPubMs = now;
          tbPublishTelemetry(tv);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void thingsboardBegin() {
  xTaskCreatePinnedToCore(tbMqttTask, "tb_mqtt", TB_TASK_STACK, nullptr,
                          1, &tbTaskHandle, TB_TASK_CORE);
}


// ---- Stage 6: SoC coulomb-counting integration --------------------------------
// Called at the end of every report round (~5.6 s). Integrates net current
// (Chg - Dis) into SoC, auto-recalibrates at the voltage endpoints, and saves
// to NVS every 5 min. All float math - takes microseconds, never blocks loop().
static void serviceSoC(float vBatt, float netA) {
  uint32_t now = millis();
  if (socLastMs == 0) { socLastMs = now; return; }   // first call: just stamp
  float dt_h = (float)(now - socLastMs) / 3600000.0f;  // ms -> hours
  socLastMs = now;

  // Coulomb integration: +netA (charging) raises SoC, -netA lowers it.
  socPct += (netA * dt_h / BAT_CAP_AH) * 100.0f;

  // Auto-recalibrate at the LiFePO4 endpoints - the only voltages that map to
  // a definite SoC. This corrects drift from sensor offset integration error.
  if (vBatt >= SOC_HI_V) socPct = 100.0f;
  if (vBatt <= SOC_LO_V) socPct = 0.0f;
  if (socPct < 0.0f)   socPct = 0.0f;
  if (socPct > 100.0f) socPct = 100.0f;

  // Persist to NVS periodically (flash wear, not every round).
  if (now - socSaveMs > SOC_SAVE_MS || socSaveMs == 0) {
    socSaveMs = now;
    prefs.putFloat(KEY_SOC, socPct);
  }
}

// 'S<%>' command: manually set SoC (e.g. after a known full charge) and save.
static void setSoc(float pct) {
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  socPct = pct;
  prefs.putFloat(KEY_SOC, socPct);
  socLastMs = millis();   // restart integration from this moment
  socSaveMs = millis();
  Serial.printf("[SoC ] set to %.1f%%, saved to NVS\n", socPct);
}

// "2026-09-15 12:34:56" once synced, else "boot+123s".
static void stampNow(char *out, size_t len) {
  if (ntpSynced) {
    time_t t = time(nullptr);
    struct tm tv; localtime_r(&t, &tv);
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &tv);
  } else {
    snprintf(out, len, "boot+%lus", (unsigned long)(millis() / 1000UL));
  }
}

// Daily file once the clock is live; one catch-all file before that.
static void logFileName(char *out, size_t len) {
  if (ntpSynced) {
    time_t t = time(nullptr);
    struct tm tv; localtime_r(&t, &tv);
    strftime(out, len, "/log_%y%m%d.csv", &tv);     // sorts chronologically
  } else {
    snprintf(out, len, "%s", "/log_boot.csv");
  }
}

// Our CSV files under "/". File::path() keeps the leading slash that
// LittleFS open/remove/exists expect. Names sort by day (YYMMDD inside).
static std::vector<String> listLogFiles() {
  std::vector<String> files;
  File root = LittleFS.open("/");
  if (!root) return files;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String n = f.path();
    if (!f.isDirectory() && n.startsWith("/log_") && n.endsWith(".csv"))
      files.push_back(n);
  }
  root.close();
  std::sort(files.begin(), files.end());
  return files;
}

// Keep only the newest LOG_KEEP_FILES logs (pruned once per boot).
static void pruneOldLogs() {
  std::vector<String> files = listLogFiles();
  if (files.size() <= LOG_KEEP_FILES) return;
  size_t del = files.size() - LOG_KEEP_FILES;
  for (size_t i = 0; i < del; i++) LittleFS.remove(files[i]);
  Serial.printf("[LOG ] pruned %u old log file(s)\n", (unsigned)del);
}

static void fsBegin() {
  fsOk = LittleFS.begin(true);                     // format on first mount
  if (!fsOk) { Serial.println("[LOG ] LittleFS mount FAILED - logging off"); return; }
  Serial.printf("[LOG ] LittleFS ok, %u/%u KB used\n",
                (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
  pruneOldLogs();
}

static void writeLogLine() {
  if (!fsOk || logAgg.n == 0) return;
  char name[24];
  logFileName(name, sizeof(name));
  bool fresh = !LittleFS.exists(name);
  File f = LittleFS.open(name, FILE_APPEND);
  if (!f) { Serial.println("[LOG ] open failed"); return; }
  if (fresh) f.println("epoch,timestamp,v_solar_V,v_batt_V,i_chg_mA,i_dis_mA,samples");
  char ts[24]; stampNow(ts, sizeof(ts));
  float n = (float)logAgg.n;
  f.printf("%ld,%s,%.3f,%.3f,%.1f,%.1f,%lu\n",
           (long)(ntpSynced ? time(nullptr) : 0), ts,
           logAgg.vSolar / n, logAgg.vBatt / n,
           logAgg.iChg / n, logAgg.iDis / n,
           (unsigned long)logAgg.n);
  f.close();
}

// Fixed cadence: one line per LOG_INTERVAL_MS; a minute with no samples is
// simply skipped (keeps timestamps honest if the ADC ever goes away).
static void serviceLogger() {
  uint32_t now = millis();
  if (now - lastLogMs < LOG_INTERVAL_MS) return;
  lastLogMs = now;
  if (logAgg.n == 0) return;
  writeLogLine();
  logAgg.reset();
}

static void dumpLogTail(size_t lines) {
  if (!fsOk) { Serial.println("[LOG ] FS not mounted"); return; }
  char name[24];
  logFileName(name, sizeof(name));
  File f = LittleFS.open(name, FILE_READ);
  if (!f) { Serial.printf("[LOG ] nothing logged yet in %s\n", name); return; }
  std::vector<String> ring;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length()) {
      ring.push_back(l);
      if (ring.size() > lines) ring.erase(ring.begin());
    }
  }
  f.close();
  Serial.printf("---- %s, last %u line(s) ----\n", name, (unsigned)ring.size());
  for (size_t i = 0; i < ring.size(); i++) Serial.println(ring[i]);
  Serial.println("---- end of dump ----");
}

static void eraseLogs() {
  if (!fsOk) { Serial.println("[LOG ] FS not mounted"); return; }
  std::vector<String> files = listLogFiles();
  for (size_t i = 0; i < files.size(); i++) LittleFS.remove(files[i]);
  Serial.printf("[LOG ] erased %u log file(s)\n", (unsigned)files.size());
  logAgg.reset();
}
#else
// ---- ADC-only build: no WiFi/NTP/FS. Provide no-op stubs so setup()/loop()
//      can call them unconditionally without #if guards at every call site.
static void wifiNtpBegin()   {}
static void serviceNetwork() {}
static void fsBegin()        {}
static void serviceLogger()  {}
#endif // ENABLE_NETWORK

// ---- Field calibration of ACS712 sensitivity (both builds) -----------------
// Cheap ACS712 modules are frequently mis-labelled (a "20A" board may carry a
// 30A die, etc), so the datasheet sensitivity can be far from reality. This
// command lets the user calibrate the TRUE sensitivity against a multimeter:
//   1) With a KNOWN current I flowing through the charge channel (A2), read
//      that current on a multimeter wired in series with the ACS712
//      (e.g. 0.948 A at the battery).
//   2) Send "C<amps>" over serial (e.g. "C0.948").
//   3) Firmware computes sens = (rawChg - zeroChg) / I and saves it to NVS; it
//      overrides the zero-derived sensitivity on every future boot.
// 'c' clears the field calibration (reverts to the zero-derived sensitivity).
static void fieldCalSensitivity(float measuredAmps) {
  if (measuredAmps <= 0.001f) {
    Serial.println("[CAL ] amps must be > 0 (use the multimeter reading while current flows)");
    return;
  }
  float dv = lastReportChg - vZero[CUR_CH[0]];
  if (fabsf(dv) < 0.005f) {
    Serial.printf("[CAL ] raw chg ch (%.4f) too close to zero (%.4f) - is current actually flowing?\n",
                  lastReportChg, vZero[CUR_CH[0]]);
    return;
  }
  float sens = fabsf(dv) / measuredAmps;        // V/A (sign handled by CUR_SIGN_FLIP)
  if (sens < 0.040f || sens > 0.250f) {
    Serial.printf("[CAL ] computed sens %.1f mV/A is out of range (40-250) - check multimeter/zero\n",
                  sens * 1000.0f);
    return;
  }
  vPerA = sens;
  prefs.putFloat(KEY_VPA_CAL, sens);
  prefs.putBool(KEY_VPA_CAL_VALID, true);
  Serial.printf("[CAL ] field sensitivity SAVED: %.1f mV/A (was %.1f). Currents now use this value.\n",
                sens * 1000.0f, prefs.getFloat(KEY_VPA, 0.100f) * 1000.0f);
}

static void clearFieldCal() {
  prefs.putBool(KEY_VPA_CAL_VALID, false);
  // Recompute the zero-derived sensitivity so the live value reverts at once.
  float vcc = vZero[CUR_CH[0]] * 2.0f;
  vPerA = ACS712_V_PER_A_AT_5V * (vcc / ACS712_VCC_NOMINAL);
  Serial.printf("[CAL ] field calibration CLEARED. sens reverted to %.1f mV/A (from zero).\n",
                vPerA * 1000.0f);
}

// 't' = clock/WiFi/IQAir/SoC status, 'd' = tail of the current log, 'E' = erase logs.
#if ENABLE_NETWORK
static void handleSerialCmd() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 't') {
      char ts[24]; stampNow(ts, sizeof(ts));
      IQAirData iq;
      bool iqOk = iqairSnapshot(iq);
      Serial.printf("[TIME] %s | NTP %s | WiFi %s | IQAir %s | SoC %.1f%%\n", ts,
                    ntpSynced ? "synced" : "waiting",
                    WiFi.status() == WL_CONNECTED ? "up" : "down",
                    iqOk ? "fetched" : "pending",
                    socPct);
      if (iqOk) {
        Serial.printf("       AQI=%d PM1=%.1f PM2.5=%.1f PM10=%.1f T=%.1fC RH=%.0f%% P=%.1fmbar\n",
                      iq.aqi_us, iq.pm1, iq.pm25, iq.pm10,
                      iq.temp_c, iq.humidity, iq.pressure);
      }
      // Stage 7: dump the cached telemetry snapshot exactly as the MQTT stage
      // will see it. getValue() returns a private copy (bounded 10 ms lock).
      TelemetryValues tv = getValue();
      if (tv.uptime_s == 0) {
        Serial.println("       [telemetry] snapshot not ready yet");
      } else {
        Serial.printf("       Bat=%s | Solar=%s | IQAir=%s | v_solar=%.2fV v_batt=%.2fV\n",
                      tv.bat_status, tv.sol_status, tv.iq_status, tv.v_solar, tv.v_batt);
        Serial.printf("       i_solar=%.3fA p_solar=%.1fW | i_load=%.3fA p_load=%.1fW\n",
                      tv.i_solar, tv.p_solar, tv.i_load, tv.p_load);
        Serial.printf("       SoC=%.1f%% SoH=%.1f%% | energy in=%.2fWh out=%.2fWh | up=%lus\n",
                      tv.soc, tv.soh, tv.energy_in_wh, tv.energy_out_wh, tv.uptime_s);
      }
    } else if (c == 'd') {
      dumpLogTail(20);
    } else if (c == 'E') {
      eraseLogs();
    } else if (c == 'i') {
      // Force an immediate IQAir fetch (runs on core 0, returns instantly).
      iqairRequestFetch();
      Serial.println("[IQAir] fetch requested (async, on core 0)");
    } else if (c == 'S') {
      // 'S<%>' sets SoC manually, e.g. "S90" -> 90%. Saves to NVS.
      String s = Serial.readStringUntil('\n');
      s.trim();
      setSoc(s.toFloat());
    } else if (c == 'C') {
      String s = Serial.readStringUntil('\n');
      s.trim();
      fieldCalSensitivity(s.toFloat());
    } else if (c == 'c') {
      clearFieldCal();
    }
  }
}
#else
// ---- ADC-only build: no WiFi/NTP/FS, serial commands limited to status ----
static void handleSerialCmd() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 't') {
      Serial.printf("[ADC ] boot+%lus | chg(A2) zero=%.4f dis(A3) zero=%.4f sens=%.1f mV/A\n",
                    (unsigned long)(millis() / 1000),
                    vZero[CUR_CH[0]], vZero[CUR_CH[1]], vPerA * 1000.0f);
    } else if (c == 'C') {
      String s = Serial.readStringUntil('\n');
      s.trim();
      fieldCalSensitivity(s.toFloat());
    } else if (c == 'c') {
      clearFieldCal();
    }
  }
}
#endif

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_PIN, r, g, b);
}

// Write a 16-bit value (big-endian) to an ADS register. Returns true on ACK.
static bool adsWrite16(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(ADS_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

// Read a 16-bit value (big-endian) from an ADS register.
static bool adsRead16(uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(ADS_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)ADS_ADDR, (uint8_t)2) != 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

// ---- Non-blocking sampler state machine ----------------------------------
// Replaces the old blocking readChAvg() loops: one ADS1115 single-shot
// conversion at a time, sequenced with millis() - loop() never busy-waits.
enum class SamplerMode : uint8_t { REPORT, ZERO };
enum class SamplerStep : uint8_t { START, WAIT, READ };

// Ascending comparison for qsort on a float array (used by trimmed mean).
static int cmpFloat(const void *a, const void *b) {
  float fa = *(const float *)a, fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

struct ChanAcc {                    // per-channel accumulator for one round
  double   sum;
  uint16_t count;
  float    lo, hi;
  // Raw samples for the current channels only (spike rejection via trimmed
  // mean). Voltage channels never trim, so they just ignore this buffer.
  float    vals[CUR_BUF_SAMPLES];
  void reset() { sum = 0; count = 0; lo = 1e9f; hi = -1e9f; }
  void add(float v) {
    if (count < CUR_BUF_SAMPLES) vals[count] = v;
    sum += v; ++count;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  bool full(uint16_t target) const { return count >= target; }
  float mean() const { return count ? (float)(sum / count) : 0.0f; }
  // Sort the raw samples ONCE and derive both the trimmed mean and the
  // trimmed peak-to-peak from the same sorted array (one qsort per call
  // instead of two). trimPct is dropped from EACH tail. Rejects transient
  // spikes (e.g. a switching load on A3) that would otherwise pull the plain
  // mean off the true steady current, while the reported "pp" reflects the
  // real noise on the steady current rather than one-off outliers.
  void computeTrimmed(uint8_t trimPct, float &outMean, float &outPp) const {
    outMean = 0.0f; outPp = 0.0f;
    if (count == 0) return;
    uint16_t n = count;
    if (n > CUR_BUF_SAMPLES) n = CUR_BUF_SAMPLES;   // defensive clamp
    float tmp[CUR_BUF_SAMPLES];
    for (uint16_t i = 0; i < n; i++) tmp[i] = vals[i];
    qsort(tmp, n, sizeof(float), cmpFloat);
    uint16_t drop = (uint16_t)((uint32_t)n * trimPct / 100);
    if ((uint16_t)(drop * 2) >= n) {                 // over-trimmed -> median
      outMean = tmp[n >> 1];
      return;
    }
    double s = 0;
    for (uint16_t i = drop; i < n - drop; i++) s += tmp[i];
    outMean = (float)(s / (double)(n - 2 * drop));
    outPp   = tmp[n - 1 - drop] - tmp[drop];
  }
};

static ChanAcc     acc[4];
static SamplerMode mode = SamplerMode::REPORT;
static SamplerStep step = SamplerStep::START;
static const uint8_t SEQ_REPORT[4] = {0, 1, 2, 3};  // round-robin, all ch
static const uint8_t SEQ_ZERO[2]   = {2, 3};        // current channels only (PCB rev: A2/A3)
static uint8_t  seqIdx       = 0;
static uint8_t  activeCh     = 0;
static uint32_t convStartMs  = 0;
static bool     adcOk        = false;  // false while I2C errors persist

static void startRound(SamplerMode m) {
  mode = m;
  step = SamplerStep::START;
  seqIdx = 0;
  for (int i = 0; i < 4; i++) acc[i].reset();
}

// Apply the accumulated zero samples (valid only with NO current flowing).
static void finishZero() {
  vZero[CUR_CH[0]] = acc[CUR_CH[0]].mean();
  vZero[CUR_CH[1]] = acc[CUR_CH[1]].mean();

  // ACS712 is ratiometric: infer VCC from the mid-rail (VCC = 2*Vzero), then
  // scale the nominal-5V sensitivity accordingly.
  float vcc = vZero[CUR_CH[0]] * 2.0f;
  vPerA = ACS712_V_PER_A_AT_5V * (vcc / ACS712_VCC_NOMINAL);
  Serial.printf("Zero chg(A%d)=%.4f V, dis(A%d)=%.4f V | VCC~=%.2f V | sens=%.1f mV/A\n",
                vZero[CUR_CH[0]], CUR_CH[0], vZero[CUR_CH[1]], CUR_CH[1],
                vcc, vPerA * 1000.0f);

  // Persist ONLY deliberate BOOT-press calibrations. The boot-time fallback
  // zero must not be stored: it may have been captured with current flowing
  // and would then poison every future boot.
  if (zeroSavePending) {
    zeroSavePending = false;
    prefs.putFloat(KEY_VZ2, vZero[CUR_CH[0]]);
    prefs.putFloat(KEY_VZ3, vZero[CUR_CH[1]]);
    prefs.putFloat(KEY_VPA, vPerA);
    prefs.putBool(KEY_VALID, true);
    Serial.println("Zero SAVED to NVS - reused on every boot until you re-zero again");
  }
}

static void finishReport() {
  // Currents shown in AMPERES (like the ACS712 vendor example); 3 decimals
  // keep milliamp-level resolution (0.001 A = 1 mA). pp = peak-to-peak noise.
  // Raw chg(A2)/dis(A3) volts are printed to verify the ACS712 front end:
  // with current flowing, they must move away from the stored zero point by
  // I*vPerA. Net = Chg - Dis = current actually entering the battery (compare
  // against a multimeter placed at the BMS->battery lead, NOT the charger feed).
  // Trimmed mean for the noisy ACS712 channels rejects transient spikes (e.g.
  // a switching load on A3) that pull the plain mean off the true steady
  // current. Each channel sorts once and we reuse the sorted array for both
  // the mean and the pp. Voltages stay on the plain mean (divider inputs have
  // no spikes).
  float chgMean, chgPp, disMean, disPp;
  acc[CUR_CH[0]].computeTrimmed(CUR_TRIM_PCT, chgMean, chgPp);
  acc[CUR_CH[1]].computeTrimmed(CUR_TRIM_PCT, disMean, disPp);
  float chgA = (chgMean - vZero[CUR_CH[0]]) / vPerA * CUR_SIGN_FLIP_CHG;
  float disA = (disMean - vZero[CUR_CH[1]]) / vPerA * CUR_SIGN_FLIP_DIS;
  float vBatt = acc[VOLT_CH[1]].mean() * voltGain[VOLT_CH[1]];
  float netA  = chgA - disA;
  Serial.printf(
    "Solar: %6.3f V  Batt: %6.3f V | Chg: %+7.3f A (pp %.3f)  "
    "Dis: %+7.3f A (pp %.3f) | Net: %+7.3f A | SoC: %5.1f%% | raw A%d=%7.4f A%d=%7.4f (zero %.4f/%.4f)\n",
    acc[VOLT_CH[0]].mean() * voltGain[VOLT_CH[0]],
    vBatt,
    chgA,
    chgPp / vPerA,
    disA,
    disPp / vPerA,
    netA,
    socPct,
    CUR_CH[0], chgMean,
    CUR_CH[1], disMean,
    vZero[CUR_CH[0]],
    vZero[CUR_CH[1]]);

  // Keep the most recent (trimmed) raw chg/dis for the 'C' field-cal command,
  // so a calibration taken against a multimeter uses the same spike-free value
  // shown in the report line.
  lastReportChg = chgMean;
  lastReportDis = disMean;

#if ENABLE_NETWORK
  // Stage 4: feed the 1-minute logging aggregator with this round's means.
  logAgg.add(acc[VOLT_CH[0]].mean() * voltGain[VOLT_CH[0]],
             vBatt,
             (chgMean - vZero[CUR_CH[0]]) / vPerA * 1000.0f * CUR_SIGN_FLIP_CHG,
             (disMean - vZero[CUR_CH[1]]) / vPerA * 1000.0f * CUR_SIGN_FLIP_DIS);
  // Stage 6: integrate SoC from this round's net current (coulomb counting).
  // Runs on core 1 (here) but is pure float math - microseconds, no I/O.
  serviceSoC(vBatt, netA);
  // Stage 7: compute derived telemetry + cache it for getValue() (MQTT stage).
  // All float math (microseconds, no I/O inside the telemetry lock). The IQAir
  // snapshot is taken and released FIRST so the two mutexes are never nested.
  float vSolar = acc[VOLT_CH[0]].mean() * voltGain[VOLT_CH[0]];
  float pSolar = vSolar * chgA;
  float pLoad  = vBatt  * disA;
  serviceEnergy(pSolar, pLoad);
  serviceSoH(chgA, disA);                 // EFC throughput + SoH (NVS write is outside the lock below)

  IQAirData iq;
  bool iqOk = iqairSnapshot(iq);          // locks/unlocks iqairMutex on its own

  xSemaphoreTake(telemetryMutex, portMAX_DELAY);
  telemetry.v_solar = vSolar;  telemetry.i_solar = chgA;  telemetry.p_solar = pSolar;
  telemetry.v_batt  = vBatt;   telemetry.soc      = socPct;
  telemetry.i_load  = disA;    telemetry.p_load   = pLoad;
  telemetry.soh     = sohPct;
  telemetry.energy_in_wh  = (float)energyInWh;
  telemetry.energy_out_wh = (float)energyOutWh;
  // NOTE: deriveSolStatus reads telemetry.bat_status, so bat_status must be
  // assigned first (it is on the line below).
  telemetry.bat_status = deriveBatStatus(chgA, disA, vBatt, socPct);
  telemetry.sol_status = deriveSolStatus(vSolar, chgA, telemetry.bat_status);
  telemetry.iq_status  = deriveIqStatus(WiFi.status() == WL_CONNECTED, iqOk);
  telemetry.uptime_s = millis() / 1000UL;
  // boot attributes are const defaults in the struct; no need to rewrite them.
  telemetry.iq_valid = iqOk;
  if (iqOk) {
    telemetry.aqi_us = iq.aqi_us; telemetry.pm1 = iq.pm1; telemetry.pm25 = iq.pm25;
    telemetry.pm10 = iq.pm10; telemetry.temp_c = iq.temp_c;
    telemetry.humidity = iq.humidity; telemetry.pressure = iq.pressure;
    telemetry.iq_ts = iq.ts;
  }
  xSemaphoreGive(telemetryMutex);
#endif
}

// I2C failure: rate-limited message, red LED, restart the current round.
static void adcError() {
  static uint32_t lastPrint = 0;
  adcOk = false;
  uint32_t now = millis();
  if (now - lastPrint >= 1000) {
    lastPrint = now;
    Serial.println("ADC read error (no ACK)");
  }
  startRound(mode);
}

// Samples needed by a channel in the current mode (voltages average less,
// noisy ACS712 channels average more; zero keeps the old 256-sample count).
static uint16_t targetFor(uint8_t ch) {
  (void)ch;                                  // targets are mode-based now
  if (mode == SamplerMode::ZERO) return ZERO_SAMPLES;
  return CH_SAMPLES;
}

// One state-machine tick: at most one I2C transaction, zero busy-waiting.
static void runSampler() {
  uint32_t now = millis();
  const uint8_t *seq = (mode == SamplerMode::ZERO) ? SEQ_ZERO : SEQ_REPORT;
  const uint8_t  n   = (mode == SamplerMode::ZERO) ? 2 : 4;

  switch (step) {
    case SamplerStep::START: {
      activeCh = seq[seqIdx];
      uint16_t config = CONFIG_BASE | ((uint16_t)(0x4 | (activeCh & 0x3)) << 12);
      if (!adsWrite16(REG_CONFIG, config)) { adcError(); return; }
      convStartMs = now;
      step = SamplerStep::WAIT;
      break;
    }

    case SamplerStep::WAIT:                        // conversion runs meanwhile
      if (now - convStartMs >= CONV_WAIT_MS) step = SamplerStep::READ;
      break;

    case SamplerStep::READ: {
      uint16_t raw = 0;
      if (!adsRead16(REG_CONVERSION, raw)) { adcError(); return; }
      acc[activeCh].add((float)(int16_t)raw * FS_VOLTS / 32768.0f);

      // Continuous round-robin: move to the NEXT channel in the sequence.
      // Equal per-channel targets mean every channel fills at the same rate
      // (one sample per pass) inside the SAME time window - no skipping, so
      // the four averages cannot desynchronise. When the channel we just
      // advanced to is already full, every channel is: round done.
      seqIdx = (seqIdx + 1) % n;
      if (acc[seq[seqIdx]].full(targetFor(seq[seqIdx]))) {
        if (mode == SamplerMode::ZERO) finishZero();
        else                           finishReport();
        adcOk = true;
        startRound(SamplerMode::REPORT);     // fresh round, back to sampling
      } else {
        step = SamplerStep::START;           // keep accumulating this round
      }
      break;
    }
  }
}

// LED colour changes only (avoid rewriting the WS2812 every loop pass).
static void updateLed() {
  static uint8_t last = 0;
  uint8_t c = (mode == SamplerMode::ZERO) ? 1 : (adcOk ? 3 : 2);
  if (c == last) return;
  last = c;
  if      (c == 1) setLed(0, 0, 40);    // BLUE : zeroing
  else if (c == 2) setLed(40, 0, 0);    // RED  : ADC not responding
  else             setLed(0, 40, 0);    // GREEN: reading OK
}

// BOOT button (active LOW), debounced without delay(): a press restarts the
// zero calibration - MUST be done with NO current through A1/A3.
static void handleBootButton() {
  static int      stable = HIGH;
  static int      lastRaw = HIGH;
  static uint32_t lastChangeMs = 0;

  int raw = digitalRead(BOOT_BTN);
  if (raw != lastRaw) { lastRaw = raw; lastChangeMs = millis(); }
  if (raw == stable || millis() - lastChangeMs < 30) return;   // debounce
  stable = raw;
  if (stable == LOW && mode != SamplerMode::ZERO) {
    Serial.println("Zeroing current channels A2,A3... (loads OFF; result saved to NVS)");
    zeroSavePending = true;
    startRound(SamplerMode::ZERO);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);   // boot only: let the USB-CDC port settle before the banner
  Serial.println();
  Serial.println("=== ESP32-S3-ETH Solar charger: A0=v_solar A1=v_batt A2=i_charge A3=i_discharge ===");
  Serial.printf("ADC 0x%02X @ SDA=%d/SCL=%d | PGA=+/-%.3fV | non-blocking sampler\n",
                ADS_ADDR, I2C_SDA, I2C_SCL, FS_VOLTS);
  Serial.println("Press BOOT (loads OFF, NO current) to RE-ZERO and save to NVS.");
  Serial.println("Stage5: WiFi+NTP+LittleFS+IQAir(fetch on core0)+SoC(coulomb). Serial: 't' status  'd' log  'E' erase  'i' IQAir  'S<%>' set SoC");

  pinMode(BOOT_BTN, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // Calibration source: a BOOT-press zero saved in NVS survives reboots, so
  // powering up with current flowing cannot fold real current into the
  // offset. With empty/invalid NVS we fall back to a boot auto-zero, which
  // is NOT saved (see finishZero()).
  prefs.begin(PREFS_NS, false);
  bool haveCal = false;
  if (prefs.getBool(KEY_VALID, false)) {
    float z2 = prefs.getFloat(KEY_VZ2, 0.0f);
    float z3 = prefs.getFloat(KEY_VZ3, 0.0f);
    float pa = prefs.getFloat(KEY_VPA, 0.100f);
    // Plausibility: ACS712 zero sits near VCC/2 (~2.5 V). Sensitivity can be
    // 66 (30A) / 100 (20A) / 185 (5A) mV/A, and a field calibration may land
    // anywhere in between, so accept a wide 40-250 mV/A window here.
    if (z2 > 2.0f && z2 < 3.0f && z3 > 2.0f && z3 < 3.0f &&
        pa > 0.040f && pa < 0.250f) {
      vZero[CUR_CH[0]] = z2; vZero[CUR_CH[1]] = z3; vPerA = pa;
      haveCal = true;
    }
  }
  // Field calibration (measured against a multimeter) overrides the zero-
  // derived sensitivity when present - it captures the TRUE sensitivity of
  // this particular ACS712 in this circuit (cheap modules are often mis-
  // labelled: a "20A" board may carry a 30A die, etc).
  bool haveFieldSens = false;
  if (prefs.getBool(KEY_VPA_CAL_VALID, false)) {
    float paCal = prefs.getFloat(KEY_VPA_CAL, 0.0f);
    if (paCal > 0.040f && paCal < 0.250f) {
      vPerA = paCal;
      haveFieldSens = true;
    }
  }
  if (haveCal) {
    Serial.printf("Using stored NVS zero: chg(A%d)=%.4f V, dis(A%d)=%.4f V, sens=%.1f mV/A%s\n",
                  CUR_CH[0], vZero[CUR_CH[0]], CUR_CH[1], vZero[CUR_CH[1]], vPerA * 1000.0f,
                  haveFieldSens ? "  [FIELD-CAL]" : "");
    startRound(SamplerMode::REPORT);       // calibration done: sample right away
  } else {
    Serial.println("No valid stored zero - boot auto-zero (NOT saved). Press BOOT with loads OFF to store one.");
    startRound(SamplerMode::ZERO);
  }

  setupWebBegin(); // SoftAP + Web UI portal (loads NVS config, starts AP)
  wifiNtpBegin();  // WiFi Station + SNTP (connects with NVS config)
  fsBegin();        // LittleFS for the periodic CSV logs

  // Load last-known SoC from NVS (coulomb counting continues from here). Falls
  // back to 50% on first boot; the endpoint recalibration will correct drift
  // once the battery reaches 3.65 V (full) or 2.50 V (empty).
  socPct = prefs.getFloat(KEY_SOC, 50.0f);
  if (socPct < 0.0f || socPct > 100.0f) socPct = 50.0f;
  socLastMs = 0;   // serviceSoC() will stamp on its first call
  socSaveMs = millis();
  Serial.printf("[SoC ] starting at %.1f%% (cap %.0f Ah, LiFePO4 %gV/%gV endpoints)\n",
                socPct, BAT_CAP_AH, SOC_HI_V, SOC_LO_V);

  // Stage 7: telemetry snapshot mutex + load SoH throughput accumulator.
  ahThroughput = prefs.getDouble(KEY_AH_TP, 0.0);
  if (ahThroughput < 0.0) ahThroughput = 0.0;
  ahLastMs = 0;    // serviceSoH() will stamp on its first call
  sohSaveMs = millis();
  telemetryMutex = xSemaphoreCreateMutex();
  Serial.printf("[SoH ] throughput %.1f Ah -> SoH ~%.1f%% (cycle-life %.0f, EOL %g%%)\n",
                ahThroughput, sohPct, CYCLE_LIFE, END_OF_LIFE_SOH);

  iqairBegin();    // start the core-0 fetch task; fetches after WiFi comes up
  thingsboardBegin();  // start the core-0 TB MQTT task
}

void loop() {
  handleBootButton();
  handleSerialCmd();
  runSampler();
  serviceNetwork();
  serviceLogger();
  updateLed();
}
