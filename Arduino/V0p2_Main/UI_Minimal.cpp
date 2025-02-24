/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2013--2016
*/

/*
 Implementation of minimal UI using single LED and one or more momentary push-buttons, etc, plus CLI.
 */

#include <util/atomic.h>

#include <Arduino.h>
#include <string.h>

#include "V0p2_Main.h"
#include <OTV0p2_Board_IO_Config.h> // I/O pin allocation and setup: include ahead of I/O module headers.
#include "V0p2_Sensors.h"
#include "Control.h"
#include "Messaging.h"
#include "UI_Minimal.h"


// Marked true if the physical UI controls are being used.
// Cleared at end of tickUI().
// Marked volatile for thread-safe lock-free non-read-modify-write access to byte-wide value.
static volatile bool statusChange;

// If non-zero then UI controls have been recently manually/locally operated; counts down to zero.
// Marked volatile for thread-safe lock-free non-read-modify-write access to byte-wide value.
// Compound operations on this value must block interrupts.
#define UI_DEFAULT_RECENT_USE_TIMEOUT_M 31
#define UI_DEFAULT_VERY_RECENT_USE_TIMEOUT_M 2
static volatile uint8_t uiTimeoutM;

// Remaining minutes to keep CLI active; zero implies inactive.
// Starts up with zero value (CLI off) to avoid taking too many startup cycles from calibration, etc.
// Marked volatile for thread-safe lock-free non-read-modify-write access to byte-wide value.
// Compound operations on this value must block interrupts.
#define CLI_DEFAULT_TIMEOUT_M 2
static volatile uint8_t CLITimeoutM;
// Reset CLI active timer to the full whack before it goes inactive again (ie makes CLI active for a while).
// Thread-safe.
void resetCLIActiveTimer() { CLITimeoutM = CLI_DEFAULT_TIMEOUT_M; }
// Returns true if the CLI is active, at least intermittently.
// Thread-safe.
bool isCLIActive() { return(0 != CLITimeoutM); }

// Record local manual operation of a physical UI control, eg neither remote nor via CLI.
// Marks room as occupied amongst other things.
// To be thread-/ISR- safe, everything that this touches or calls must be.
// Thread-safe.
void markUIControlUsed()
  {
  statusChange = true; // Note user interaction with the system.
  uiTimeoutM = UI_DEFAULT_RECENT_USE_TIMEOUT_M; // Ensure that UI controls are kept 'warm' for a little while.
#if defined(ENABLE_UI_WAKES_CLI)
  // Make CLI active for a while (at some slight possibly-significant energy cost).
  resetCLIActiveTimer(); // Thread-safe.
#endif
  // User operation of controls locally is strong indication of presence.
  Occupancy.markAsOccupied(); // Thread-safe.
  }

// Set true on significant local UI operation.
// Should be cleared when feedback has been given.
// Marked volatile for thread-safe lockless access;
static volatile bool significantUIOp;
// Record significant local manual operation of a physical UI control, eg not remote or via CLI.
// Marks room as occupied amongst other things.
// As markUIControlUsed() but likely to generate some feedback to the user, ASAP.
// Thread-safe.
void markUIControlUsedSignificant()
  {
  // Provide some instant visual feedback if possible.
  LED_HEATCALL_ON_ISR_SAFE();
  // Flag up need for feedback.
  significantUIOp = true;
  // Do main UI-touched work.
  markUIControlUsed();
  }

// True if a manual UI control has been very recently (minutes ago) operated.
// The user may still be interacting with the control and the UI etc should probably be extra responsive.
// Thread-safe.
bool veryRecentUIControlUse() { return(uiTimeoutM >= (UI_DEFAULT_RECENT_USE_TIMEOUT_M - UI_DEFAULT_VERY_RECENT_USE_TIMEOUT_M)); }

// True if a manual UI control has been recently (tens of minutes ago) operated.
// If true then local manual settings should 'win' in any conflict with programmed or remote ones.
// For example, remote requests to override settings may be ignored while this is true.
// Thread-safe....
bool recentUIControlUse() { return(0 != uiTimeoutM); }

// UI feedback.
// Provide low-key visual / audio / tactile feedback on a significant user action.
// May take hundreds of milliseconds and noticeable energy.
// By default includes visual feedback,
// but that can be prevented if other visual feedback already in progress.
// Marks the UI as used.
// Not thread-/ISR- safe.
void userOpFeedback(bool includeVisual)
  {
  if(includeVisual) { LED_HEATCALL_ON(); }
  markUIControlUsed();
#if defined(ENABLE_LOCAL_TRV) && defined(ENABLE_V1_DIRECT_MOTOR_DRIVE)
  // Sound and tactile feedback with local valve, like mobile phone vibrate mode.
  // Only do this if in a normal state, eg not calibrating nor in error.
  if(ValveDirect.isInNormalRunState()) { ValveDirect.wiggle(); }
    else
#else
  // In absence of being all-in-one, or as else where valve cannot be used...
  // pause briefly to let LED on be seen.
    { if(includeVisual) { smallPause(); } }
#endif
  if(includeVisual) { LED_HEATCALL_OFF(); }
  // Note that feedback for significant UI action has been given.
  significantUIOp = false;
  }


#ifdef ENABLE_LEARN_BUTTON
// Handle learn button(s).
// First/primary button is 0, second is 1, etc.
// In simple mode: if in frost mode clear simple schedule else set repeat for every 24h from now.
// May be called from pushbutton or CLI UI components.
static void handleLEARN(const uint8_t which)
  {
  // Set simple schedule starting every 24h from a little before now and running for an hour or so.  
  if(inWarmMode()) { Scheduler.setSimpleSchedule(OTV0P2BASE::getMinutesSinceMidnightLT(), which); }
  // Clear simple schedule.
  else { Scheduler.clearSimpleSchedule(which); }
  }
#endif // ENABLE_LEARN_BUTTON


// Pause between flashes to allow them to be distinguished (>100ms); was mediumPause() for PICAXE V0.09 impl.
static void inline offPause()
  {
  bigPause(); // 120ms, was V0.09 144ms mediumPause() for PICAXE V0.09 impl.
  pollIO(); // Slip in an I/O poll.
  }

// Counts calls to tickUI.
static uint8_t tickCount;

// Call this on even numbered seconds (with current time in seconds) to allow the UI to operate.
// Should never be skipped, so as to allow the UI to remain responsive.
// Runs in 350ms or less; usually takes only a few milliseconds or microseconds.
// Returns true iff the user interacted with the system, and maybe caused a status change.
// NOTE: since this is on the minimum idle-loop code path, minimise CPU cycles, esp in frost mode.
// Also re-activates CLI on main button push.
#ifndef NO_UI_SUPPORT
bool tickUI(const uint_fast8_t sec)
  {
  // Perform any once-per-minute operations.
  const bool sec0 = (0 == sec);
  if(sec0)
    {
    ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
      {
      // Run down UI interaction timer if need be, one tick per minute.
      if(uiTimeoutM > 0) { --uiTimeoutM; }
      }
    }

#ifdef ENABLE_OCCUPANCY_SUPPORT
  const bool reportedRecently = Occupancy.reportedRecently();
#else
  const bool reportedRecently = false;
#endif
// Drive second UI LED if available.
#if defined(LED_UI2_EXISTS) && defined(ENABLE_UI_LED_2_IF_AVAILABLE)
  // Flash 2nd UI LED very briefly every 'tick' while activity has recently been reported.
  if(reportedRecently) { LED_UI2_ON(); veryTinyPause(); }
  LED_UI2_OFF(); // Generally force 2nd LED off.
#endif

  // True on every 4th tick/call, ie about once every 8 seconds.
  const bool forthTick = !((++tickCount) & 3); // True on every 4th tick.

  // Provide enhanced feedback when the has been very recent interaction with the UI,
  // since the user is still quite likely to be continuing.
  const bool enhancedUIFeedback = veryRecentUIControlUse();

#ifdef TEMP_POT_AVAILABLE
  // Force relatively-frequent re-read of temp pot UI device periodically
  // and if there has been recent UI maunal activity,
  // to keep the valve UI responsive.
#if !defined(ENABLE_FAST_TEMP_POT_SAMPLING) || !defined(ENABLE_OCCUPANCY_SUPPORT)
  if(enhancedUIFeedback || forthTick)
#else
  // Even more responsive at some possible energy cost...
  // Polls pot on every tick unless the room has been vacant for a day or two or is in FROST mode.
  if(enhancedUIFeedback || forthTick || (inWarmMode() && !Occupancy.longLongVacant()))
#endif
    {
    TempPot.read();
#if 0 && defined(DEBUG)
      DEBUG_SERIAL_PRINT_FLASHSTRING("TP");
      DEBUG_SERIAL_PRINT(TempPot.getRaw());
      DEBUG_SERIAL_PRINTLN();
#endif   
    // Force to FROST mode (and cancel any erroneous BAKE, etc) when at FROST end of dial.
    const bool isLo = TempPot.isAtLoEndStop();
    if(isLo) { setWarmModeDebounced(false); }  
    // Feed back significant change in pot position, ie at temperature boundaries.
    // Synthesise a 'warm' target temp that distinguishes end stops...
    const uint8_t nominalWarmTarget = isLo ? 1 :
        (TempPot.isAtHiEndStop() ? 99 :
        getWARMTargetC());
    // Record of 'last' nominalWarmTarget; initially 0.
    static uint8_t lastNominalWarmTarget;
    if(nominalWarmTarget != lastNominalWarmTarget)
      {
      // Note if a boundary was crossed, ignoring any false 'start-up' transient.
      if(0 != lastNominalWarmTarget) { significantUIOp = true; }
#if 1 && defined(DEBUG)
      DEBUG_SERIAL_PRINT_FLASHSTRING("WT");
      DEBUG_SERIAL_PRINT(nominalWarmTarget);
      DEBUG_SERIAL_PRINTLN();
#endif
      lastNominalWarmTarget = nominalWarmTarget;
      }
    }
#endif

  // Provide extra user feedback for significant UI actions...
  if(significantUIOp) { userOpFeedback(); }

#if !defined(ENABLE_SIMPLIFIED_MODE_BAKE)
  // Full MODE button behaviour:
  //   * cycle through FROST/WARM/BAKE while held down
  //   * switch to selected mode on release
  //
  // If true then is in WARM (or BAKE) mode; defaults to (starts as) false/FROST.
  // Should be only be set when 'debounced'.
  // Defaults to (starts as) false/FROST.
  static bool isWarmModePutative;
  static bool isBakeModePutative;

  static bool modeButtonWasPressed;
  if(fastDigitalRead(BUTTON_MODE_L) == LOW)
    {
    if(!modeButtonWasPressed)
      {
      // Capture real mode variable as button is pressed.
      isWarmModePutative = inWarmMode();
      isBakeModePutative = inBakeMode();
      modeButtonWasPressed = true;
      }

    // User is pressing the mode button: cycle through FROST | WARM [ | BAKE ].
    // Mark controls used and room as currently occupied given button press.
    markUIControlUsed();
    // LED on...
    LED_HEATCALL_ON();
    tinyPause(); // Leading tiny pause...
    if(!isWarmModePutative) // Was in FROST mode; moving to WARM mode.
      {
      isWarmModePutative = true;
      isBakeModePutative = false;
      // 2 x flash 'heat call' to indicate now in WARM mode.
      LED_HEATCALL_OFF();
      offPause();
      LED_HEATCALL_ON();
      tinyPause();
      }
    else if(!isBakeModePutative) // Was in WARM mode, move to BAKE (with full timeout to run).
      {
      isBakeModePutative = true;
      // 2 x flash + one longer flash 'heat call' to indicate now in BAKE mode.
      LED_HEATCALL_OFF();
      offPause();
      LED_HEATCALL_ON();
      tinyPause();
      LED_HEATCALL_OFF();
      mediumPause(); // Note different flash on/off duty cycle to try to distinguish this last flash.
      LED_HEATCALL_ON();
      mediumPause();
      }
    else // Was in BAKE (if supported, else was in WARM), move to FROST.
      {
      isWarmModePutative = false;
      isBakeModePutative = false;
      // 1 x flash 'heat call' to indicate now in FROST mode.
      }
    }
  else
#endif // !defined(ENABLE_SIMPLIFIED_MODE_BAKE)
    {
#if !defined(ENABLE_SIMPLIFIED_MODE_BAKE)
    // Update real control variables for mode when button is released.
    if(modeButtonWasPressed)
      {
      // Don't update the debounced WARM mode while button held down.
      // Will also capture programmatic changes to isWarmMode, eg from schedules.
      const bool isWarmModeDebounced = isWarmModePutative;
      setWarmModeDebounced(isWarmModeDebounced);
      if(isBakeModePutative) { startBake(); } else { cancelBakeDebounced(); }

      markUIControlUsed(); // Note activity on release of MODE button...
      modeButtonWasPressed = false;
      }
#endif // !defined(ENABLE_SIMPLIFIED_MODE_BAKE)

    // Keep reporting UI status if the user has just touched the unit in some way or UI feedback is enhanced.
    const bool justTouched = statusChange || enhancedUIFeedback;

    // Mode button not pressed: indicate current mode with flash(es); more flashes if actually calling for heat.
    // Force display while UI controls are being used, eg to indicate temp pot position.
    if(justTouched || inWarmMode()) // Generate flash(es) if in WARM mode or fiddling with UI other than Mode button.
      {
      // DHD20131223: only flash if the room not known to be dark so as to save energy and avoid disturbing sleep, etc.
      // In this case force resample of light level frequently in case user turns light on eg to operate unit.
      // Do show LED flash if user has recently operated controls (other than mode button) manually.
      // Flash infrequently if no recently operated controls and not in BAKE mode and not actually calling for heat;
      // this is to conserve batteries for those people who leave the valves in WARM mode all the time.
      if(justTouched ||
         ((forthTick
#if defined(ENABLE_NOMINAL_RAD_VALVE) && defined(ENABLE_LOCAL_TRV)
             || NominalRadValve.isCallingForHeat()
#endif
             || inBakeMode()) && !AmbLight.isRoomDark()))
        {
        // First flash to indicate WARM mode (or pot being twiddled).
        LED_HEATCALL_ON();
        // LED on stepwise proportional to temp pot setting.
        // Small number of steps (3) should help make positioning more obvious.
        const uint8_t wt = getWARMTargetC();
        // Makes vtiny|tiny|medium flash for cool|OK|warm temperature target.
        // Stick to minimum length flashes to save energy unless just touched.
        if(!justTouched || isEcoTemperature(wt)) { veryTinyPause(); }
        else if(!isComfortTemperature(wt)) { tinyPause(); }
        else { mediumPause(); }

#if defined(ENABLE_NOMINAL_RAD_VALVE) && defined(ENABLE_LOCAL_TRV)
        // Second flash to indicate actually calling for heat,
        // or likely to be calling for heat while interacting with the controls, to give fast user feedback (TODO-695).
        if((enhancedUIFeedback && NominalRadValve.isUnderTarget()) ||
            NominalRadValve.isCallingForHeat() ||
            inBakeMode())
          {
          LED_HEATCALL_OFF();
          offPause(); // V0.09 was mediumPause().
          LED_HEATCALL_ON(); // flash
          // Stick to minimum length flashes to save energy unless just touched.
          if(!justTouched || isEcoTemperature(wt)) { veryTinyPause(); }
          else if(!isComfortTemperature(wt)) { OTV0P2BASE::sleepLowPowerMs((VERYTINY_PAUSE_MS + TINY_PAUSE_MS) / 2); }
          else { tinyPause(); }

          if(inBakeMode())
            {
            // Third (lengthened) flash to indicate BAKE mode.
            LED_HEATCALL_OFF();
            mediumPause(); // Note different flash off time to try to distinguish this last flash.
            LED_HEATCALL_ON();
            // Makes tiny|small|medium flash for eco|OK|comfort temperature target.
            // Stick to minimum length flashes to save energy unless just touched.
            if(!justTouched || isEcoTemperature(wt)) { veryTinyPause(); }
            else if(!isComfortTemperature(wt)) { smallPause(); }
            else { mediumPause(); }
            }
          }
#endif
        }
      }
 
#if defined(ENABLE_NOMINAL_RAD_VALVE) && defined(ENABLE_LOCAL_TRV)
    // Even in FROST mode, and if actually calling for heat (eg opening the rad valve significantly, etc)
    // then emit a tiny double flash on every 4th tick.
    // This call for heat may be frost protection or pre-warming / anticipating demand.
    // DHD20130528: new 4th-tick flash in FROST mode...
    // DHD20131223: only flash if the room is not dark (ie or if no working light sensor) so as to save energy and avoid disturbing sleep, etc.
    else if(forthTick &&
            !AmbLight.isRoomDark() &&
            NominalRadValve.isCallingForHeat() /* &&
            NominalRadValve.isControlledValveReallyOpen() */ )
      {
      // Double flash every 4th tick indicates call for heat while in FROST MODE (matches call for heat in WARM mode).
      LED_HEATCALL_ON(); // flash
      veryTinyPause();
      LED_HEATCALL_OFF();
      offPause();
      LED_HEATCALL_ON(); // flash
      veryTinyPause();
      }
#endif

    // Enforce any changes that may have been driven by other UI components (ie other than MODE button).
    // Eg adjustment of temp pot / eco bias changing scheduled state.
    if(statusChange)
      {
      static bool prevScheduleStatus;
      const bool currentScheduleStatus = Scheduler.isAnyScheduleOnWARMNow();
      if(currentScheduleStatus != prevScheduleStatus)
        {
        prevScheduleStatus = currentScheduleStatus;
        setWarmModeDebounced(currentScheduleStatus);
        }
      }
    }

  // Ensure LED forced off unconditionally at least once each cycle.
  LED_HEATCALL_OFF();

#ifdef ENABLE_LEARN_BUTTON
  // Handle learn button if supported and if is currently pressed.
  if(fastDigitalRead(BUTTON_LEARN_L) == LOW)
    {
    handleLEARN(0);
    userOpFeedback(false); // Mark controls used and room as currently occupied given button press.
    LED_HEATCALL_ON(); // Leave heatcall LED on while learn button held down.
    }

#if defined(BUTTON_LEARN2_L)
  // Handle second learn button if supported and currently pressed and primary learn button not pressed.
  else if(fastDigitalRead(BUTTON_LEARN2_L) == LOW)
    {
    handleLEARN(1);
    userOpFeedback(false); // Mark controls used and room as currently occupied given button press.
    LED_HEATCALL_ON(); // Leave heatcall LED on while learn button held down.
    }
#endif
#endif

  const bool statusChanged = statusChange;
  statusChange = false; // Potential race.
  return(statusChanged);
  }
#endif // tickUI


// Check/apply the user's schedule, at least once each minute, and act on any timed events.
#if !defined(checkUserSchedule)
void checkUserSchedule()
  {
  // Get minutes since midnight local time [0,1439].
  const uint_least16_t msm = OTV0P2BASE::getMinutesSinceMidnightLT();

  // Check all available schedules.
  // FIXME: probably will NOT work as expected for overlapping schedules (ie will got to FROST at end of first one).
  for(uint8_t which = 0; which < Scheduler.MAX_SIMPLE_SCHEDULES; ++which)
    {
    // Check if now is the simple scheduled off time, as minutes after midnight [0,1439]; invalid (eg ~0) if none set.
    // Programmed off/frost takes priority over on/warm if same to bias towards energy-saving.
    // Note that in the presence of multiple overlapping schedules only the last 'off' applies however.
    if(((Scheduler.MAX_SIMPLE_SCHEDULES < 1) || !Scheduler.isAnyScheduleOnWARMNow()) &&
       (msm == Scheduler.getSimpleScheduleOff(which)))
      { setWarmModeDebounced(false); }
    // Check if now is the simple scheduled on time.
    else if(msm == Scheduler.getSimpleScheduleOn(which))
      { setWarmModeDebounced(true); }
    }
  }
#endif // !defined(checkUserSchedule)


#ifdef ENABLE_EXTENDED_CLI
// Handle CLI extension commands.
// Commands of form:
//   +EXT .....
// where EXT is the name of the extension, usually 3 letters.

#include <OTProtocolCC.h>

// It is acceptable for extCLIHandler() to alter the buffer passed,
// eg with strtok_t().
static bool extCLIHandler(Print *const p, char *const buf, const uint8_t n)
  {
  return(false); // FAILED if not otherwise handled.
  }
#endif 


// Prints a single space to Serial (which must be up and running).
static void Serial_print_space() { Serial.print(' '); }

#if defined(ENABLE_SERIAL_STATUS_REPORT) && !defined(serialStatusReport)
// Sends a short 1-line CRLF-terminated status report on the serial connection (at 'standard' baud).
// Ideally should be similar to PICAXE V0.1 output to allow the same parser to handle either.
// Will turn on UART just for the duration of this call if powered off.
// Has multiple sections, some optional, starting with a unique letter and separated with ';'.
/*
Status output may look like this...
=F0%@18C;T16 36 W255 0 F255 0;S5 5 17
=W0%@18C;T16 38 W255 0 F255 0;S5 5 17
=W0%@18C;T16 39 W255 0 F255 0;S5 5 17
=W0%@18C;T16 40 W16 39 F17 39;S5 5 17
=W0%@18C;T16 41 W16 39 F17 39;S5 5 17
=W0%@17C;T16 42 W16 39 F17 39;S5 5 17
=W20%@17C;T16 43 W16 39 F17 39;S5 5 17
=W20%@17C;T16 44 W16 39 F17 39;S5 5 17
=F0%@17C;T16 45 W16 39 F17 39;S5 5 17

When driving an FHT8V wireless radiator valve it may look like this:
=F0%@18C;T2 30 W10 0 F12 0;S5 5 17 wf;HC255 255
=F0%@18C;T2 30 W10 0 F12 0;S5 5 17 wf;HC255 255
=W0%@18C;T2 31 W10 0 F12 0;S5 5 17 wf;HC255 255
=W10%@18C;T2 32 W10 0 F12 0;S5 5 17 wf;HC255 255
=W20%@18C;T2 33 W10 0 F12 0;S5 5 17 wfo;HC255 255

'=' starts the status line and CRLF ends it; sections are separated with ";".
The initial 'W' or 'F' is WARM or FROST mode indication.  (If BAKE mode is supported, 'B' may be shown instead of 'W' when in BAKE.)
The nn% is the target valve open percentage.
The @nnCh gives the current measured room temperature in (truncated, not rounded) degrees C, followed by hex digit for 16ths.
The ";" terminates this initial section.
Thh mm is the local current 24h time in hours and minutes.
Whh mm is the scheduled on/warm time in hours and minutes, or an invalid time if none.
Fhh mm is the scheduled off/frost time in hours and minutes, or an invalid time if none.
The ";" terminates this schedule section.
'S' introduces the current and settable-target temperatures in Celsius/centigrade, if supported.
eg 'S5 5 17'
The first number is the current target in C, the second is the FROST target, the third is the WARM target.
The 'e' or 'c' indicates eco or comfort bias.
A 'w' indicates that this hour is predicted for smart warming ('f' indicates not), and another 'w' the hour ahead.
A trailing 'o' indicates room occupancy.
The ";" terminates this 'settable' section.

'HC' introduces the optional FHT8V house codes section, if supported and codes are set.
eg 'HC99 99'
HChc1 hc2 are the house codes 1 and 2 for an FHT8V valve.
*/
void serialStatusReport()
  {
  const bool neededWaking = OTV0P2BASE::powerUpSerialIfDisabled<V0P2_UART_BAUD>(); // FIXME

  // Aim to overlap CPU usage with characters being TXed for throughput determined primarily by output size and baud.

  // Stats line starts with distinguished marker character.
  // Initial '=' section with common essentials.
  Serial.print((char) OTV0P2BASE::SERLINE_START_CHAR_STATS);
//#ifdef SUPPORT_BAKE
  Serial.print(inWarmMode() ? (inBakeMode() ? 'B' : 'W') : 'F');
//#else
//  Serial.print(inWarmMode() ? 'W' : 'F');
//#endif
#if defined(ENABLE_NOMINAL_RAD_VALVE)
  Serial.print(NominalRadValve.get()); Serial.print('%'); // Target valve position.
#endif
  const int temp = TemperatureC16.get();
  Serial.print('@'); Serial.print(temp >> 4); Serial.print('C'); // Unrounded whole degrees C.
      Serial.print(temp & 0xf, HEX); // Show 16ths in hex.

//#if 0
//  // *P* section: low power flag shown only if (battery) low.
//  if(Supply_mV.isSupplyVoltageLow()) { Serial.print(F(";Plow")); }
//#endif

#ifdef ENABLE_FULL_OT_CLI
  // *X* section: Xmit security level shown only if some non-essential TX potentially allowed.
  const OTV0P2BASE::stats_TX_level xmitLevel = OTV0P2BASE::getStatsTXLevel();
  if(xmitLevel < OTV0P2BASE::stTXnever) { Serial.print(F(";X")); Serial.print(xmitLevel); }
#endif

#ifdef ENABLE_FULL_OT_CLI
  // *T* section: time and schedules.
  const uint_least8_t hh = OTV0P2BASE::getHoursLT();
  const uint_least8_t mm = OTV0P2BASE::getMinutesLT();
  Serial.print(';'); // End previous section.
  Serial.print('T'); Serial.print(hh); Serial_print_space(); Serial.print(mm);
#if defined(SCHEDULER_AVAILABLE)
  // Show all schedules set.
  for(uint8_t scheduleNumber = 0; scheduleNumber < Scheduler.MAX_SIMPLE_SCHEDULES; ++scheduleNumber)
    {
    Serial_print_space();
    uint_least16_t startMinutesSinceMidnightLT = Scheduler.getSimpleScheduleOn(scheduleNumber);
    const bool invalidStartTime = startMinutesSinceMidnightLT >= OTV0P2BASE::MINS_PER_DAY;
    const int startH = invalidStartTime ? 255 : (startMinutesSinceMidnightLT / 60);
    const int startM = invalidStartTime ? 0 : (startMinutesSinceMidnightLT % 60);
    Serial.print('W'); Serial.print(startH); Serial_print_space(); Serial.print(startM);
    Serial_print_space();
    uint_least16_t endMinutesSinceMidnightLT = Scheduler.getSimpleScheduleOff(scheduleNumber);
    const bool invalidEndTime = endMinutesSinceMidnightLT >= OTV0P2BASE::MINS_PER_DAY;
    const int endH = invalidEndTime ? 255 : (endMinutesSinceMidnightLT / 60);
    const int endM = invalidEndTime ? 0 : (endMinutesSinceMidnightLT % 60);
    Serial.print('F'); Serial.print(endH); Serial_print_space(); Serial.print(endM);
    }
  if(Scheduler.isAnyScheduleOnWARMNow()) { Serial.print('*'); } // Indicate that at least one schedule is active now.
#endif // ENABLE_SINGLETON_SCHEDULE
#endif

  // *S* section: settable target/threshold temperatures, current target, and eco/smart/occupied flags.
#ifdef ENABLE_SETTABLE_TARGET_TEMPERATURES // Show thresholds and current target since no longer so easily deduced.
  Serial.print(';'); // Terminate previous section.
  Serial.print('S'); // Current settable temperature target, and FROST and WARM settings.
#ifdef ENABLE_LOCAL_TRV
  Serial.print(NominalRadValve.getTargetTempC());
#endif // ENABLE_LOCAL_TRV
  Serial_print_space();
  Serial.print(getFROSTTargetC());
  Serial_print_space();
  const uint8_t wt = getWARMTargetC();
  Serial.print(wt);
#ifdef ENABLE_FULL_OT_CLI
  // Show bias.
  Serial_print_space();
  Serial.print(hasEcoBias() ? (isEcoTemperature(wt) ? 'E' : 'e') : (isComfortTemperature(wt) ? 'C': 'c')); // Show eco/comfort bias.
#endif // ENABLE_FULL_OT_CLI
#endif // ENABLE_SETTABLE_TARGET_TEMPERATURES

  // *C* section: central hub values.
#if defined(ENABLE_BOILER_HUB) || defined(ENABLE_STATS_RX)
  // Print optional hub boiler-on-time section if apparently set (non-zero) and thus in hub mode.
  const uint8_t boilerOnMinutes = getMinBoilerOnMinutes();
  if(boilerOnMinutes != 0)
    {
    Serial.print(';'); // Terminate previous section.
    Serial.print('C'); // Indicate central hub mode available.
    Serial.print(boilerOnMinutes); // Show min 'on' time, or zero if disabled.
    }
#endif

  // *H* section: house codes for local FHT8V valve and if syncing, iff set.
#if defined(ENABLE_FHT8VSIMPLE)
  // Print optional house code section if codes set.
  const uint8_t hc1 = FHT8V.nvGetHC1();
  if(hc1 != 255)
    {
    Serial.print(F(";HC"));
    Serial.print(hc1);
    Serial_print_space();
    Serial.print(FHT8V.nvGetHC2());
    if(!FHT8V.isInNormalRunState())
      {
      Serial_print_space();
      Serial.print('s'); // Indicate syncing with trailing lower-case 's' in field...
      }
    }
#endif

#if defined(ENABLE_LOCAL_TRV) && !defined(ENABLE_TRIMMED_MEMORY)
  // *M* section: min-valve-percentage open section, iff not at default value.
  const uint8_t minValvePcOpen = NominalRadValve.getMinValvePcReallyOpen();
  if(OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN != minValvePcOpen) { Serial.print(F(";M")); Serial.print(minValvePcOpen); }
#endif

#if 1 && defined(ENABLE_JSON_OUTPUT) && !defined(ENABLE_TRIMMED_MEMORY)
  Serial.print(';'); // Terminate previous section.
  char buf[40]; // Keep short enough not to cause overruns.
  static const uint8_t maxStatsLineValues = 5;
  static OTV0P2BASE::SimpleStatsRotation<maxStatsLineValues> ss1; // Configured for maximum different stats.
//  ss1.put(TemperatureC16); // Already at start of = stats line.
#if defined(HUMIDITY_SENSOR_SUPPORT)
  ss1.put(RelHumidity);
#endif // defined(HUMIDITY_SENSOR_SUPPORT)
#if defined(ENABLE_AMBLIGHT_SENSOR)
  ss1.put(AmbLight);
#endif // ENABLE_AMBLIGHT_SENSOR
  ss1.put(Supply_cV);
#if defined(ENABLE_OCCUPANCY_SUPPORT)
  ss1.put(Occupancy);
//  ss1.put(Occupancy.vacHTag(), Occupancy.getVacancyH()); // EXPERIMENTAL
#endif // defined(ENABLE_OCCUPANCY_SUPPORT)
#if defined(ENABLE_MODELLED_RAD_VALVE) && !defined(ENABLE_TRIMMED_MEMORY)
    ss1.put(NominalRadValve.tagCMPC(), NominalRadValve.getCumulativeMovementPC()); // EXPERIMENTAL
#endif // ENABLE_MODELLED_RAD_VALVE
  const uint8_t wrote = ss1.writeJSON((uint8_t *)buf, sizeof(buf), 0, true);
  if(0 != wrote) { Serial.print(buf); }
#endif // defined(ENABLE_JSON_OUTPUT) && !defined(ENABLE_TRIMMED_MEMORY)

  // Terminate line.
  Serial.println();

  // Ensure that all text is sent before this routine returns, in case any sleep/powerdown follows that kills the UART.
  OTV0P2BASE::flushSerialSCTSensitive();

  if(neededWaking) { OTV0P2BASE::powerDownSerial(); }
  }
#endif // defined(ENABLE_SERIAL_STATUS_REPORT) && !defined(serialStatusReport)

#if defined(ENABLE_CLI_HELP) && !defined(ENABLE_TRIMMED_MEMORY)
#define _CLI_HELP_
#define SYNTAX_COL_WIDTH 10 // Width of 'syntax' column; strictly positive.
// Estimated maximum overhead in sub-cycle ticks to print full line and all trailing CLI summary info.
#define CLI_PRINT_OH_SCT ((uint8_t)(OTV0P2BASE::GSCT_MAX/4))
// Deadline in minor cycle by which to stop printing description.
#define STOP_PRINTING_DESCRIPTION_AT ((uint8_t)(OTV0P2BASE::GSCT_MAX-CLI_PRINT_OH_SCT))
// Efficiently print a single line given the syntax element and the description, both non-null.
// NOTE: will skip the description if getting close to the end of the time deadline, in order to avoid overrun.
static void printCLILine(const uint8_t deadline, __FlashStringHelper const *syntax, __FlashStringHelper const *description)
  {
  Serial.print(syntax);
  OTV0P2BASE::flushSerialProductive(); // Ensure all pending output is flushed before sampling current position in minor cycle.
  if(OTV0P2BASE::getSubCycleTime() >= deadline) { Serial.println(); return; }
  for(int8_t padding = SYNTAX_COL_WIDTH - strlen_P((const char *)syntax); --padding >= 0; ) { Serial_print_space(); }
  Serial.println(description);
  }
// Efficiently print a single line given a single-char syntax element and the description, both non-null.
// NOTE: will skip the description if getting close to the end of the time deadline to avoid overrun.
static void printCLILine(const uint8_t deadline, const char syntax, __FlashStringHelper const *description)
  {
  Serial.print(syntax);
  OTV0P2BASE::flushSerialProductive(); // Ensure all pending output is flushed before sampling current position in minor cycle.
  if(OTV0P2BASE::getSubCycleTime() >= deadline) { Serial.println(); return; }
  for(int8_t padding = SYNTAX_COL_WIDTH - 1; --padding >= 0; ) { Serial_print_space(); }
  Serial.println(description);
  }
#endif // defined(ENABLE_CLI_HELP) && !defined(ENABLE_TRIMMED_MEMORY)

// Dump some brief CLI usage instructions to serial TX, which must be up and running.
// If this gets too big there is a risk of overrunning and missing the next tick...
static void dumpCLIUsage(const uint8_t stopBy)
  {
#ifndef _CLI_HELP_
  OTV0P2BASE::CLI::InvalidIgnored(); // Minimal placeholder.
#else
  const uint8_t deadline = OTV0P2BASE::fnmin((uint8_t)(stopBy - OTV0P2BASE::fnmin(stopBy,CLI_PRINT_OH_SCT)), STOP_PRINTING_DESCRIPTION_AT);
  Serial.println();
  //Serial.println(F("CLI usage:"));
  printCLILine(deadline, '?', F("this help"));
  
  // Core CLI features first... (E, [H], I, S V)
  printCLILine(deadline, 'E', F("Exit CLI"));
#if defined(ENABLE_FHT8VSIMPLE) && defined(ENABLE_LOCAL_TRV)
  printCLILine(deadline, F("H H1 H2"), F("set FHT8V House codes 1&2"));
  printCLILine(deadline, 'H', F("clear House codes"));
#endif
  printCLILine(deadline, F("I *"), F("create new ID"));
  printCLILine(deadline, 'S', F("show Status"));
  printCLILine(deadline, 'V', F("sys Version"));
#ifdef ENABLE_GENERIC_PARAM_CLI_ACCESS
  printCLILine(deadline, F("G N [M]"), F("Show [set] generic param N [to M]")); // *******
#endif

#ifdef ENABLE_FULL_OT_CLI
  // Optional CLI features...
  Serial.println(F("-"));
#if defined(ENABLE_BOILER_HUB) || defined(ENABLE_STATS_RX)
  printCLILine(deadline, F("C M"), F("Central hub >=M mins on, 0 off"));
#endif
  printCLILine(deadline, F("D N"), F("Dump stats set N"));
  printCLILine(deadline, 'F', F("Frost"));
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES) && !defined(TEMP_POT_AVAILABLE)
  printCLILine(deadline, F("F CC"), F("set Frost/setback temp CC"));
#endif

  //printCLILine(deadline, 'L', F("Learn to warm every 24h from now, clear if in frost mode, schedule 0"));
#if defined(SCHEDULER_AVAILABLE)
  printCLILine(deadline, F("L S"), F("Learn daily warm now, clear if in frost mode, schedule S"));
  //printCLILine(deadline, F("P HH MM"), F("Program: warm daily starting at HH MM schedule 0"));
  printCLILine(deadline, F("P HH MM S"), F("Program: warm daily starting at HH MM schedule S"));
#endif
  printCLILine(deadline, F("O PP"), F("min % for valve to be Open"));
#if defined(ENABLE_NOMINAL_RAD_VALVE)
  printCLILine(deadline, 'O', F("reset Open %"));
#endif
  printCLILine(deadline, 'Q', F("Quick Heat"));
//  printCLILine(deadline, F("R N"), F("dump Raw stats set N"));

  printCLILine(deadline, F("T HH MM"), F("set 24h Time"));
  printCLILine(deadline, 'W', F("Warm"));
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES) && !defined(TEMP_POT_AVAILABLE)
  printCLILine(deadline, F("W CC"), F("set Warm temp CC"));
#endif
#if !defined(ENABLE_ALWAYS_TX_ALL_STATS)
  printCLILine(deadline, 'X', F("Xmit security level; 0 always, 255 never"));
#endif
  printCLILine(deadline, 'Z', F("Zap stats"));
#endif // ENABLE_FULL_OT_CLI
#endif // ENABLE_CLI_HELP
  Serial.println();
  }

#if defined(ENABLE_EXTENDED_CLI) || defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
static const uint8_t MAXIMUM_CLI_RESPONSE_CHARS = 1 + OTV0P2BASE::CLI::MAX_TYPICAL_CLI_BUFFER;
#else
static const uint8_t MAXIMUM_CLI_RESPONSE_CHARS = 1 + OTV0P2BASE::CLI::MIN_TYPICAL_CLI_BUFFER;
#endif
// Used to poll user side for CLI input until specified sub-cycle time.
// Commands should be sent terminated by CR *or* LF; both may prevent 'E' (exit) from working properly.
// A period of less than (say) 500ms will be difficult for direct human response on a raw terminal.
// A period of less than (say) 100ms is not recommended to avoid possibility of overrun on long interactions.
// Times itself out after at least a minute or two of inactivity. 
// NOT RENTRANT (eg uses static state for speed and code space).
void pollCLI(const uint8_t maxSCT, const bool startOfMinute)
  {
  // Perform any once-per-minute operations.
  if(startOfMinute)
    {
    ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
      {
      // Run down CLI timer if need be.
      if(CLITimeoutM > 0) { --CLITimeoutM; }
      }
    }

  const bool neededWaking = OTV0P2BASE::powerUpSerialIfDisabled<V0P2_UART_BAUD>();

  // Wait for input command line from the user (received characters may already have been queued)...
  // Read a line up to a terminating CR, either on its own or as part of CRLF.
  // (Note that command content and timing may be useful to fold into PRNG entropy pool.)
  static char buf[MAXIMUM_CLI_RESPONSE_CHARS+1]; // Note: static state, efficient for small command lines.  Space for terminating '\0'.
  const uint8_t n = OTV0P2BASE::CLI::promptAndReadCommandLine(maxSCT, buf, sizeof(buf), burnHundredsOfCyclesProductivelyAndPoll);

  if(n > 0)
    {
    // Got plausible input so keep the CLI awake a little longer.
    resetCLIActiveTimer();

    // Process the input received, with action based on the first char...
    bool showStatus = true; // Default to showing status.
    switch(buf[0])
      {
      // Explicit request for help, or unrecognised first character.
      // Avoid showing status as may already be rather a lot of output.
      default: case '?': { dumpCLIUsage(maxSCT); showStatus = false; break; }

      // Exit/deactivate CLI immediately.
      // This should be followed by JUST CR ('\r') OR LF ('\n')
      // else the second will wake the CLI up again.
      case 'E': { CLITimeoutM = 0; break; }

#if defined(ENABLE_FHT8VSIMPLE) && (defined(ENABLE_LOCAL_TRV) || defined(ENABLE_SLAVE_TRV))
      // H [nn nn]
      // Set (non-volatile) HC1 and HC2 for single/primary FHT8V wireless valve under control.
      // Missing values will clear the code entirely (and disable use of the valve).
      case 'H': { showStatus = OTRadValve::FHT8VRadValveBase::SetHouseCode(&FHT8V).doCommand(buf, n); break; }
#endif

#if defined(ENABLE_GENERIC_PARAM_CLI_ACCESS)
      // Show/set generic parameter values (eg "G N [M]").
      case 'G': { showStatus = OTV0P2BASE::CLI::GenericParam().doCommand(buf, n); break; }
#endif

      // Set or display new random ID.
      case 'I': { showStatus = OTV0P2BASE::CLI::NodeID().doCommand(buf, n); break; }

      // Status line and optional smart/scheduled warming prediction request.
      case 'S':
        {
#if !defined(ENABLE_WATCHDOG_SLOW)
        Serial.print(F("Resets/overruns: "));
#else
        Serial.print(F("Resets: "));
#endif // !defined(ENABLE_WATCHDOG_SLOW) 
        const uint8_t resetCount = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT);
        Serial.print(resetCount);
#if !defined(ENABLE_WATCHDOG_SLOW)
        Serial.print(' ');
        const uint8_t overrunCount = (~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER)) & 0xff;
        Serial.print(overrunCount);
#endif // !defined(ENABLE_WATCHDOG_SLOW)
        Serial.println();
        break; // Note that status is by default printed after processing input line.
        }

#if !defined(ENABLE_TRIMMED_MEMORY)
      // Version information printed as one line to serial, machine- and human- parseable.
      case 'V':
        {
        serialPrintlnBuildVersion();
#if defined(DEBUG) && defined(ENABLE_EXTENDED_CLI) // && !defined(ENABLE_TRIMMED_MEMORY)
        // Allow for much longer input commands for extended CLI.
        Serial.print(F("Ext CLI max chars: ")); Serial.println(MAXIMUM_CLI_RESPONSE_CHARS);
#endif
        break;
        }
#endif // !defined(ENABLE_TRIMMED_MEMORY)

#ifdef ENABLE_EXTENDED_CLI
      // Handle CLI extension commands.
      // Command of form:
      //   +EXT .....
      // where EXT is the name of the extension, usually 3 letters.
      //
      // It is acceptable for extCLIHandler() to alter the buffer passed,
      // eg with strtok_t().
      case '+':
        {
        const bool success = extCLIHandler(&Serial, buf, n);
        Serial.println(success ? F("OK") : F("FAILED"));
        break;
        }
#endif 

#ifdef ENABLE_FULL_OT_CLI // *******  NON-CORE CLI FEATURES

#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT) && (defined(ENABLE_BOILER_HUB) || defined(ENABLE_STATS_RX)) && defined(ENABLE_RADIO_RX)
      // Set new node association (nodes to accept frames from).
      // Only needed if able to RX and/or some sort of hub.
      case 'A': { showStatus = OTV0P2BASE::CLI::SetNodeAssoc().doCommand(buf, n); break; }
#endif // ENABLE_OTSECUREFRAME_ENCODING_SUPPORT

#if defined(ENABLE_RADIO_RX) && (defined(ENABLE_BOILER_HUB) || defined(ENABLE_STATS_RX)) && !defined(ENABLE_DEFAULT_ALWAYS_RX)
      // C M
      // Set central-hub boiler minimum on (and off) time; 0 to disable.
      case 'C':
        {
        char *last; // Used by strtok_r().
        char *tok1;
        // Minimum 3 character sequence makes sense and is safe to tokenise, eg "C 0".
        if((n >= 3) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          {
          const uint8_t m = (uint8_t) atoi(tok1);
          setMinBoilerOnMinutes(m);
          }
        break;
        }
#endif

//      // Raw stats: R N
//      // Avoid showing status afterwards as may already be rather a lot of output.
//      case 'R':
//        {
//        char *last; // Used by strtok_r().
//        char *tok1;
//        // Minimum 3 character sequence makes sense and is safe to tokenise, eg "R 0".
//        if((n >= 3) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
//          {
//          const uint8_t setN = (uint8_t) atoi(tok1);
//          for(uint8_t hh = 0; hh < 24; ++hh)
//            { Serial.print(getByHourStat(setN, hh)); Serial_print_space(); }
//          Serial.println();
//          }
//        break;
//        }

#if !defined(ENABLE_TRIMMED_MEMORY)
      // Dump (human-friendly) stats: D N
      case 'D': { showStatus = OTV0P2BASE::CLI::DumpStats().doCommand(buf, n); break; }
#endif

#if defined(ENABLE_LOCAL_TRV)
      // Switch to FROST mode OR set FROST/setback temperature (even with temp pot available).
      // With F! force to frost and holiday (long-vacant) mode.  Useful for testing and for remote CLI use.
      case 'F':
        {
#if defined(ENABLE_OCCUPANCY_SUPPORT) && !defined(ENABLE_TRIMMED_MEMORY)
        if((n == 2) && ('!' == buf[1]))
          {
          Serial.println(F("hols"));
          Occupancy.setHolidayMode();
          }
#endif
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES)
        char *last; // Used by strtok_r().
        char *tok1;
        if((n >= 3) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          {
          const uint8_t tempC = (uint8_t) atoi(tok1);
          if(!setFROSTTargetC(tempC)) { OTV0P2BASE::CLI::InvalidIgnored(); }
          }
        else
#endif
          { setWarmModeDebounced(false); } // No parameter supplied; switch to FROST mode.
        break;
        }
#endif // defined(ENABLE_LOCAL_TRV)
 
#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      // Set secret key.
      case 'K': { showStatus = OTV0P2BASE::CLI::SetSecretKey(OTRadioLink::SimpleSecureFrame32or0BodyTXV0p2::resetRaw3BytePersistentTXRestartCounterCond).doCommand(buf, n); break; }
#endif // ENABLE_OTSECUREFRAME_ENCODING_SUPPORT

#ifdef ENABLE_LEARN_BUTTON
      // Learn current settings, just as if primary/specified LEARN button had been pressed.
      case 'L':
        {
        int s = 0;
//#if MAX_SIMPLE_SCHEDULES > 1
        char *last; // Used by strtok_r().
        char *tok1;
        // Minimum 3 character sequence makes sense and is safe to tokenise, eg "L 0".
        if((n >= 3) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          {
          s = atoi(tok1);
          }
//#endif
        handleLEARN((uint8_t) s); break;
        break;
        }
#endif // ENABLE_LEARN_BUTTON

#if defined(ENABLE_NOMINAL_RAD_VALVE) && !defined(ENABLE_TRIMMED_MEMORY)
      // Set/clear min-valve-open-% threshold override.
      case 'O':
        {
        uint8_t minPcOpen = 0; // Will clear override and use default threshold.
        char *last; // Used by strtok_r().
        char *tok1;
        if((n > 1) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          { minPcOpen = (uint8_t) atoi(tok1); }
        NominalRadValve.setMinValvePcReallyOpen(minPcOpen);
        break;
        }
#endif

#ifdef ENABLE_LEARN_BUTTON
      // Program simple schedule HH MM [N].
      case 'P':
        {
        char *last; // Used by strtok_r().
        char *tok1;
        // Minimum 5 character sequence makes sense and is safe to tokenise, eg "P 1 2".
        if((n >= 5) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          {
          char *tok2 = strtok_r(NULL, " ", &last);
          if(NULL != tok2)
            {
            const int hh = atoi(tok1);
            const int mm = atoi(tok2);
            int s = 0;
//#if MAX_SIMPLE_SCHEDULES > 1         
            char *tok3 = strtok_r(NULL, " ", &last);
            if(NULL != tok3)
              {
              s = atoi(tok3);
              }
//#endif
            // Does not fully validate user inputs (eg for -ve values), but cannot set impossible values.
            if(!Scheduler.setSimpleSchedule((uint_least16_t) ((60 * hh) + mm), (uint8_t)s)) { OTV0P2BASE::CLI::InvalidIgnored(); }
            }
          }
        break;
        }
#endif // ENABLE_LEARN_BUTTON

#if defined(ENABLE_LOCAL_TRV) && !defined(ENABLE_TRIMMED_MEMORY)
      // Switch to (or restart) BAKE (Quick Heat) mode: Q
      // We can live without this if very short of memory.
      case 'Q': { startBake(); break; }
#endif

#if !defined(ENABLE_TRIMMED_MEMORY)
      // Time set T HH MM.
      case 'T': { showStatus = OTV0P2BASE::CLI::SetTime().doCommand(buf, n); break; }
#endif // !defined(ENABLE_TRIMMED_MEMORY)

#if defined(ENABLE_LOCAL_TRV)
      // Switch to WARM (not BAKE) mode OR set WARM temperature.
      case 'W':
        {
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES) && !defined(TEMP_POT_AVAILABLE)
        char *last; // Used by strtok_r().
        char *tok1;
        if((n >= 3) && (NULL != (tok1 = strtok_r(buf+2, " ", &last))))
          {
          const uint8_t tempC = (uint8_t) atoi(tok1);
          if(!setWARMTargetC(tempC)) { OTV0P2BASE::CLI::InvalidIgnored(); }
          }
        else
#endif
          {
          cancelBakeDebounced(); // Ensure BAKE mode not entered.
          setWarmModeDebounced(true); // No parameter supplied; switch to WARM mode.
          }
        break;
        }
#endif // defined(ENABLE_LOCAL_TRV)

#if !defined(ENABLE_ALWAYS_TX_ALL_STATS)
      // TX security/privacy level: X NN
      // Avoid showing status afterwards as may already be rather a lot of output.
      case 'X': { showStatus = OTV0P2BASE::CLI::SetTXPrivacy().doCommand(buf, n); break; }
#endif

#if defined(ENABLE_LOCAL_TRV)
      // Zap/erase learned statistics.
      case 'Z': { showStatus = OTV0P2BASE::CLI::ZapStats().doCommand(buf, n); break; }
#endif // defined(ENABLE_LOCAL_TRV)

#endif // ENABLE_FULL_OT_CLI // NON-CORE FEATURES
      }

    // Almost always show status line afterwards as feedback of command received and new state.
    if(showStatus) { serialStatusReport(); }
    // Else show ack of command received.
    else { Serial.println(F("OK")); }
    }
  else { Serial.println(); } // Terminate empty/partial CLI input line after timeout.

  // Force any pending output before return / possible UART power-down.
  OTV0P2BASE::flushSerialSCTSensitive();

  if(neededWaking) { OTV0P2BASE::powerDownSerial(); }
  }
