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
                           Deniz Erbilgin 2015
*/

/*
 Control/model for TRV and boiler.
 */
#include <util/atomic.h>

#include "V0p2_Main.h"

#include "Control.h"

#include "V0p2_Sensors.h"
#include "UI_Minimal.h"

#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT) || defined(ENABLE_SECURE_RADIO_BEACON)
#include <OTAESGCM.h>
#endif


#if defined(SCHEDULER_AVAILABLE)
// Singleton scheduler instance.
SimpleValveSchedule Scheduler;
#endif


#ifdef ENABLE_BOILER_HUB
// True if boiler should be on.
static bool isBoilerOn();
#endif

// If true then is in WARM (or BAKE) mode; defaults to (starts as) false/FROST.
// Should be only be set when 'debounced'.
// Defaults to (starts as) false/FROST.
// Marked volatile to allow atomic access from ISR without a lock.
static volatile bool isWarmMode;
// If true then the unit is in 'warm' (heating) mode, else 'frost' protection mode.
bool inWarmMode() { return(isWarmMode); }
// Has the effect of forcing the warm mode to the specified state immediately.
// Should be only be called once 'debounced' if coming from a button press for example.
// If forcing to FROST mode then any pending BAKE time is cancelled.
void setWarmModeDebounced(const bool warm)
  {
  isWarmMode = warm;
  if(!warm) { cancelBakeDebounced(); }
  }
// Start/cancel WARM mode in one call, driven by manual UI input.
static void setWarmModeFromManualUI(const bool warm)
  {
  // Give feedback when changing WARM mode.
  if(inWarmMode() != warm) { markUIControlUsedSignificant(); }
  // Now set/cancel WARM.
  setWarmModeDebounced(warm);
  }

// Only relevant if isWarmMode is true.
// Marked volatile to allow atomic access from ISR without a lock; decrements should lock out interrupts.
static volatile uint_least8_t bakeCountdownM;
// If true then the unit is in 'BAKE' mode, a subset of 'WARM' mode which boosts the temperature target temporarily.
// ISR-safe.
bool inBakeMode() { return(isWarmMode && (0 != bakeCountdownM)); }
// Should be only be called once 'debounced' if coming from a button press for example.
// Cancel 'bake' mode if active; does not force to FROST mode.
void cancelBakeDebounced() { bakeCountdownM = 0; }
// Start/restart 'BAKE' mode and timeout.
// Should ideally be only be called once 'debounced' if coming from a button press for example.
// Is thread-/ISR- safe.
void startBake() { isWarmMode = true; bakeCountdownM = BAKE_MAX_M; }
#if defined(ENABLE_SIMPLIFIED_MODE_BAKE)
// Start BAKE from manual UI interrupt; marks UI as used also.
// Vetos switch to BAKE mode if a temp pot/dial is present and at the low end stop, ie in FROST position.
// Is thread-/ISR- safe.
static void startBakeFromInt()
  {
#ifdef TEMP_POT_AVAILABLE
  // Veto if dial is at FROST position.
  const bool isLo = TempPot.isAtLoEndStop(); // ISR-safe.
  if(isLo) { markUIControlUsed(); return; }
#endif
  startBake();
  markUIControlUsedSignificant();
  }
#endif // defined(ENABLE_SIMPLIFIED_MODE_BAKE)
// Start/cancel BAKE mode in one call, driven by manual UI input.
void setBakeModeFromManualUI(const bool start)
  {
  // Give feedback when changing BAKE mode.
  if(inBakeMode() != start) { markUIControlUsedSignificant(); }
  // Now set/cancel BAKE.
  if(start) { startBake(); } else { cancelBakeDebounced(); }
  }



#if defined(UNIT_TESTS)
// Support for unit tests to force particular apparent WARM setting (without EEPROM writes).
//enum _TEST_basetemp_override
//  {
//    _btoUT_normal = 0, // No override
//    _btoUT_min, // Minimum settable/reasonable temperature.
//    _btoUT_mid, // Medium settable/reasonable temperature.
//    _btoUT_max, // Minimum settable/reasonable temperature.
//  };
// Current override state; 0 (default) means no override.
static _TEST_basetemp_override _btoUT_override;
// Set the override value (or remove the override).
void _TEST_set_basetemp_override(const _TEST_basetemp_override override)
  { _btoUT_override = override; }
#endif


// Get 'FROST' protection target in C; no higher than getWARMTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
#if defined(TEMP_POT_AVAILABLE)
// Derived from temperature pot position.
uint8_t getFROSTTargetC()
  {
  // Prevent falling to lowest frost temperature if relative humidity is high (eg to avoid mould).
  const uint8_t result = (!hasEcoBias() || (RelHumidity.isAvailable() && RelHumidity.isRHHighWithHyst())) ? BIASCOM_FROST : BIASECO_FROST;
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES)
  const uint8_t stored = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_FROST_C);
  // If stored value is set and in bounds and higher than computed value then use stored value instead.
  if((stored >= MIN_TARGET_C) && (stored <= MAX_TARGET_C) && (stored > result)) { return(stored); }
#endif
  return(result);
  }
#elif defined(ENABLE_SETTABLE_TARGET_TEMPERATURES)
// Note that this value is non-volatile (stored in EEPROM).
uint8_t getFROSTTargetC()
  {
  // Get persisted value, if any.
  const uint8_t stored = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_FROST_C);
  // If out of bounds or no stored value then use default.
  if((stored < MIN_TARGET_C) || (stored > MAX_TARGET_C)) { return(FROST); }
  // TODO-403: cannot use hasEcoBias() with RH% as that would cause infinite recursion!
  // Return valid persisted value.
  return(stored);
  }
#else
#define getFROSTTargetC() ((uint8_t)FROST) // Fixed value.
#endif

// Get 'WARM' target in C; no lower than getFROSTTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
#if defined(TEMP_POT_AVAILABLE)
// Derived from temperature pot position, 0 for coldest (most eco), 255 for hottest (comfort).
// Temp ranges from eco-1C to comfort+1C levels across full (reduced jitter) [0,255] pot range.
// Everything beyond the lo/hi end-stop thresholds is forced to the appropriate end temperature.
// May be fastest computing values at the extreme ends of the range.
// Exposed for unit testing.
uint8_t computeWARMTargetC(const uint8_t pot, const uint8_t loEndStop, const uint8_t hiEndStop)
  {
#if defined(V0p2_REV)
#if 7 == V0p2_REV // Must match DORM1 scale 1+7+1 position scale FROST|16|17|18|19|20|21|22|BOOST.
#if (16 != TEMP_SCALE_MIN) || (22 != TEMP_SCALE_MAX)
#error Temperature scale must run from 16 to 22 inclusive for REV7 / DORM1 unit.
#endif
#endif
#endif

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("cWT(): ");
  DEBUG_SERIAL_PRINT(pot);
  DEBUG_SERIAL_PRINTLN();
#endif

  // Everything in the end-stop regions is assigned to the appropriate end temperature.
  // As a tiny optimisation we note that the in-scale end points must be the end temperatures also.
  if(pot <= loEndStop) { return(TEMP_SCALE_MIN); } // At/near bottom...
  if(pot >= hiEndStop) { return(TEMP_SCALE_MAX); } // At/near top...

  // Allow actual full temp range between low and high end points,
  // plus possibly a little more wiggle-room / manufacturing tolerance.
  // Range is number of actual distinct temperatures on scale between end-stop regions.
  const uint8_t usefulScale = hiEndStop - loEndStop + 1;
#define DIAL_TEMPS (TEMP_SCALE_MAX - TEMP_SCALE_MIN + 1)
  const uint8_t range = DIAL_TEMPS;
#if 7 == DIAL_TEMPS
  // REV7 / DORM1 case, with usefulScale ~ 47 as of 20160212 on first sample unit.
#define DIAL_TEMPS_SHIM
  const uint8_t rangeUsed = 8;
  const uint8_t band = (usefulScale+4) >> 3; // Width of band for each degree C...
#else
  // General case.
  const uint8_t rangeUsed = range;
  const uint8_t band = (usefulScale+(rangeUsed/2)) / rangeUsed; // Width of band for each degree C...
#endif

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("cWT(): ");
  DEBUG_SERIAL_PRINT(pot);
  DEBUG_SERIAL_PRINT(' ');
  DEBUG_SERIAL_PRINT(loEndStop);
  DEBUG_SERIAL_PRINT(' ');
  DEBUG_SERIAL_PRINT(hiEndStop);
  DEBUG_SERIAL_PRINTLN();
#endif

  // Adjust for actual bottom of useful range...
  const uint8_t ppotBasic = pot - loEndStop;
#ifndef DIAL_TEMPS_SHIM
  const uint8_t ppot = ppotBasic;
#else
  const uint8_t shim = (band >> 1);
  if(ppotBasic <= shim) { return(TEMP_SCALE_MIN); }
  const uint8_t ppot = ppotBasic - shim; // Shift up by half a slot... (using n temps in space for n+1)
#endif

  // If there are is relatively small number of distinct temperature values
  // then compute the result iteratively...
#if DIAL_TEMPS < 10
    {
    uint8_t result = TEMP_SCALE_MIN;
    uint8_t bottomOfNextBand = band;
    while((ppot >= bottomOfNextBand) && (result < TEMP_SCALE_MAX))
      {
      ++result;
      bottomOfNextBand += band;
      }
    return(result);
    }
#else  // ...else do it in one step with a division.
  return((ppot / band) + TEMP_SCALE_MIN); // Intermediate (requires expensive run-time division).
#endif
  }

// Exposed implementation.
// Uses cache to avoid expensive recomputation.
// NOT safe in face of interrupts.
uint8_t getWARMTargetC()
  {
#if defined(UNIT_TESTS)
  // Special behaviour for unit tests.
  switch(_btoUT_override)
    {
    case _btoUT_min: return(TEMP_SCALE_MIN);
    case _btoUT_mid: return(TEMP_SCALE_MID);
    case _btoUT_max: return(TEMP_SCALE_MAX);
    }
#endif

  const uint8_t pot = TempPot.get();

  // Cached input and result values; initially zero.
  static uint8_t potLast;
  static uint8_t resultLast;
  // Force recomputation if pot value changed
  // or apparently no calc done yet (unlikely/impossible zero cached result).
  if((potLast != pot) || (0 == resultLast))
    {
    const uint8_t result = computeWARMTargetC(pot, TempPot.loEndStop, TempPot.hiEndStop);
    // Cache input/result.
    resultLast = result;
    potLast = pot;
    return(result);
    }

  // Return cached result.
  return(resultLast);
  }
#elif defined(ENABLE_SETTABLE_TARGET_TEMPERATURES)
// Note that this value is non-volatile (stored in EEPROM).
uint8_t getWARMTargetC()
  {
#if defined(UNIT_TESTS)
  // Special behaviour for unit tests.
  switch(_btoUT_override)
    {
    case _btoUT_min: return(TEMP_SCALE_MIN);
    case _btoUT_mid: return(TEMP_SCALE_MID);
    case _btoUT_max: return(TEMP_SCALE_MAX);
    }
#endif

  // Get persisted value, if any.
  const uint8_t stored = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_WARM_C);
  // If out of bounds or no stored value then use default (or frost value if set and higher).
  if((stored < MIN_TARGET_C) || (stored > MAX_TARGET_C)) { return(OTV0P2BASE::fnmax((uint8_t)WARM, getFROSTTargetC())); }
  // Return valid persisted value (or frost value if set and higher).
  return(OTV0P2BASE::fnmax(stored, getFROSTTargetC()));
  }
#else
uint8_t getWARMTargetC() { return((uint8_t) (WARM)); } // Fixed value.
#endif

#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES)
// Set (non-volatile) 'FROST' protection target in C; no higher than getWARMTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
// Can also be used, even when a temperature pot is present, to set a floor setback temperature.
// Returns false if not set, eg because outside range [MIN_TARGET_C,MAX_TARGET_C], else returns true.
bool setFROSTTargetC(uint8_t tempC)
  {
  if((tempC < MIN_TARGET_C) || (tempC > MAX_TARGET_C)) { return(false); } // Invalid temperature.
  if(tempC > getWARMTargetC()) { return(false); } // Cannot set above WARM target.
  OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_FROST_C, tempC); // Update in EEPROM if necessary.
  return(true); // Assume value correctly written.
  }
#endif
#if defined(ENABLE_SETTABLE_TARGET_TEMPERATURES) && !defined(TEMP_POT_AVAILABLE)
// Set 'WARM' target in C; no lower than getFROSTTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
// Returns false if not set, eg because below FROST setting or outside range [MIN_TARGET_C,MAX_TARGET_C], else returns true.
bool setWARMTargetC(uint8_t tempC)
  {
  if((tempC < MIN_TARGET_C) || (tempC > MAX_TARGET_C)) { return(false); } // Invalid temperature.
  if(tempC < getFROSTTargetC()) { return(false); } // Cannot set below FROST target.
  OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_WARM_C, tempC); // Update in EEPROM if necessary.
  return(true); // Assume value correctly written.
  }
#endif


// If true (the default) then the system has an 'Eco' energy-saving bias, else it has a 'comfort' bias.
// Several system parameters are adjusted depending on the bias,
// with 'eco' slanted toward saving energy, eg with lower target temperatures and shorter on-times.
#ifndef hasEcoBias // If not a macro...
// True if WARM temperature at/below halfway mark between eco and comfort levels.
// Midpoint should be just in eco part to provide a system bias toward eco.
bool hasEcoBias() { return(getWARMTargetC() <= TEMP_SCALE_MID); }
//#endif
#endif


#ifndef getMinBoilerOnMinutes
// Get minimum on (and off) time for pointer (minutes); zero if not in hub mode.
uint8_t getMinBoilerOnMinutes() { return(~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_MIN_BOILER_ON_MINS_INV)); }
#endif

#ifndef setMinBoilerOnMinutes
// Set minimum on (and off) time for pointer (minutes); zero to disable hub mode.
// Suggested minimum of 4 minutes for gas combi; much longer for heat pumps for example.
void setMinBoilerOnMinutes(uint8_t mins) { OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_MIN_BOILER_ON_MINS_INV, ~(mins)); }
#endif


#ifdef ENABLE_OCCUPANCY_SUPPORT
// Singleton implementation for entire node.
OccupancyTracker Occupancy;
// Single generic occupancy callback for occupied for this instance.
void genericMarkAsOccupied() { Occupancy.markAsOccupied(); }
// Single generic occupancy callback for 'possibly occupied' for this instance.
void genericMarkAsPossiblyOccupied() { Occupancy.markAsPossiblyOccupied(); }
#endif


#ifdef ENABLE_MODELLED_RAD_VALVE
// Internal model of controlled radiator valve position.
ModelledRadValve NominalRadValve;
// Cache initially unset.
uint8_t ModelledRadValve::mVPRO_cache = 0;

// Return minimum valve percentage open to be considered actually/significantly open; [1,100].
// At the boiler hub this is also the threshold percentage-open on eavesdropped requests that will call for heat.
// If no override is set then OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN is used.
// NOTE: raising this value temporarily (and shutting down the boiler immediately if possible) is one way to implement dynamic demand.
uint8_t ModelledRadValve::getMinValvePcReallyOpen()
  {
  if(0 != mVPRO_cache) { return(mVPRO_cache); } // Return cached value if possible.
  const uint8_t stored = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_MIN_VALVE_PC_REALLY_OPEN);
  const uint8_t result = ((stored > 0) && (stored <= 100)) ? stored : OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN;
  mVPRO_cache = result; // Cache it.
  return(result);
  }

// Set and cache minimum valve percentage open to be considered really open.
// Applies to local valve and, at hub, to calls for remote calls for heat.
// Any out-of-range value (eg >100) clears the override and OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN will be used.
void ModelledRadValve::setMinValvePcReallyOpen(const uint8_t percent)
  {
  if((percent > 100) || (percent == 0) || (percent == OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN))
    {
    // Bad / out-of-range / default value so erase stored value if not already so.
    OTV0P2BASE::eeprom_smart_erase_byte((uint8_t *)V0P2BASE_EE_START_MIN_VALVE_PC_REALLY_OPEN);
    // Cache logical default value.
    mVPRO_cache = OTRadValve::DEFAULT_VALVE_PC_MIN_REALLY_OPEN;
    return;
    }
  // Store specified value with as low wear as possible.
  OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_MIN_VALVE_PC_REALLY_OPEN, percent);
  // Cache it.
  mVPRO_cache = percent;
  }

// True if the controlled physical valve is thought to be at least partially open right now.
// If multiple valves are controlled then is this true only if all are at least partially open.
// Used to help avoid running boiler pump against closed valves.
// The default is to use the check the current computed position
// against the minimum open percentage.
bool ModelledRadValve::isControlledValveReallyOpen() const
  {
  if(isRecalibrating()) { return(false); }
#ifdef ENABLE_FHT8VSIMPLE
  if(!FHT8V.isControlledValveReallyOpen()) { return(false); }
#endif
  return(value >= getMinPercentOpen());
  }
  
// Returns true if (re)calibrating/(re)initialising/(re)syncing.
// The target valve position is not lost while this is true.
// By default there is no recalibration step.
bool ModelledRadValve::isRecalibrating() const
  {
#ifdef ENABLE_FHT8VSIMPLE
  if(!FHT8V.isInNormalRunState()) { return(true); }
#endif
  return(false);
  }

// If possible exercise the valve to avoid pin sticking and recalibrate valve travel.
// Default does nothing.
void ModelledRadValve::recalibrate()
  {
#ifdef ENABLE_FHT8VSIMPLE
  FHT8V.resyncWithValve(); // Should this be decalcinate instead/also/first?
#endif
  }


// Compute target temperature (stateless).
// Can be called as often as required though may be slow/expensive.
// Will be called by computeCallForHeat().
// One aim is to allow reasonable energy savings (10--30%+)
// even if the device is left in WARM mode all the time,
// using occupancy/light/etc to determine when temperature can be set back
// without annoying users.
//
// Attempts in WARM mode to make the deepest reasonable cuts to maximise savings
// when the room is vacant and not likely to become occupied again soon,
// ie this looks ahead to give the room time to recover to target before occupancy.
//
// TODO: unit tests confirming that it is possible to reach all setback levels other than at highest comfort settings.
uint8_t ModelledRadValve::computeTargetTemp()
  {
  // In FROST mode.
  if(!inWarmMode())
    {
    const uint8_t frostC = getFROSTTargetC();

    // If scheduled WARM is due soon then ensure that room is at least at setback temperature
    // to give room a chance to hit the target, and for furniture and surfaces to be warm, etc, on time.
    // Don't do this if the room has been vacant for a long time (eg so as to avoid pre-warm being higher than WARM ever).
    // Don't do this if there has been recent manual intervention, eg to allow manual 'cancellation' of pre-heat (TODO-464).
    // Only do this if the target WARM temperature is NOT an 'eco' temperature (ie very near the bottom of the scale).
    // If well into the 'eco' zone go for a larger-than-usual setback, else go for usual small setback.
    // Note: when pre-warm and warm time for schedule is ~1.5h, and default setback 1C,
    // this is assuming that the room temperature can be raised by ~1C/h.
    // See the effect of going from 2C to 1C setback: http://www.earth.org.uk/img/20160110-vat-b.png
    // (A very long pre-warm time may confuse or distress users, eg waking them in the morning.)
    if(!Occupancy.longVacant() && Scheduler.isAnyScheduleOnWARMSoon() && !recentUIControlUse())
      {
      const uint8_t warmTarget = getWARMTargetC();
      // Compute putative pre-warm temperature, usually only just below WARM target.
      const uint8_t preWarmTempC = OTV0P2BASE::fnmax((uint8_t)(warmTarget - (isEcoTemperature(warmTarget) ? SETBACK_ECO : SETBACK_DEFAULT)), frostC);
      if(frostC < preWarmTempC) // && (!isEcoTemperature(warmTarget)))
        { return(preWarmTempC); }
      }

    // Apply FROST safety target temperature by default in FROST mode.
    return(frostC);
    }

  else if(inBakeMode()) // If in BAKE mode then use elevated target.
    {
    return(OTV0P2BASE::fnmin((uint8_t)(getWARMTargetC() + BAKE_UPLIFT), (uint8_t)MAX_TARGET_C)); // No setbacks apply in BAKE mode.
    }

  else // In 'WARM' mode with possible setback.
    {
    const uint8_t wt = getWARMTargetC();

#if defined(ENABLE_SETBACK_LOCKOUT_COUNTDOWN)
    // If smart setbacks are locked out then return WARM temperature as-is.  (TODO-786)
    if(0xff != eeprom_read_byte((uint8_t *)OTV0P2BASE::V0P2BASE_EE_START_SETBACK_LOCKOUT_COUNTDOWN_H_INV))
      {
      OTV0P2BASE::serialPrintlnAndFlush("?SLO");
      return(wt);
      }
#endif

    // Set back target the temperature a little if the room seems to have been vacant for a long time (TODO-107)
    // or it is too dark for anyone to be active or the room is not likely occupied at this time
    // or the room was apparently not occupied at thus time yesterday (and is not now).
    //   AND no WARM schedule is active now (TODO-111)
    //   AND no recent manual interaction with the unit's local UI (TODO-464) indicating local settings override.
    // The notion of "not likely occupied" is "not now"
    // AND less likely than not at this hour of the day AND an hour ahead (TODO-758).
    // Note that this mainly has to work in domestic settings in winter (with ~8h of daylight)
    // but should ideally also work in artificially-lit offices (maybe ~12h continuous lighting).
    // No 'lights-on' signal for a whole day is a fairly strong indication that the heat can be turned down.
    // TODO-451: TODO-453: ignore a short lights-off, eg from someone briefly leaving room or a transient shadow.
    // TODO: consider bottom quartile of ambient light as alternative setback trigger for near-continuously-lit spaces (aiming to spot daylight signature).
    // Look ahead to next time period (as well as current) to determine notLikelyOccupiedSoon
    // but suppress lookahead of occupancy when its been dark for many hours (eg overnight) to avoid disturbing/waking.  (TODO-792)
    // Note that deeper setbacks likely offer more savings than faster (but shallower) setbacks.
    const bool longLongVacant = Occupancy.longLongVacant();
    const bool longVacant = longLongVacant || Occupancy.longVacant();
    const bool likelyVacantNow = longVacant || Occupancy.isLikelyUnoccupied();
    const bool ecoBias = hasEcoBias();
    // True if the room has been dark long enough to indicate night.  (TODO-792)
    const uint8_t dm = AmbLight.getDarkMinutes();
    const bool darkForHours = dm > 245; // A little over 4h, not quite max 255.
    // Be more ready to decide room not likely occupied soon if eco-biased.
    // Note that this value is likely to be used +/- 1 so must be in range [1,23].
    const uint8_t thisHourNLOThreshold = ecoBias ? 15 : 12;
    const uint8_t hoursLessOccupiedThanThis = OTV0P2BASE::countStatSamplesBelow(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, OTV0P2BASE::getByHourStat(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, OTV0P2BASE::STATS_SPECIAL_HOUR_CURRENT_HOUR));
    const uint8_t hoursLessOccupiedThanNext = OTV0P2BASE::countStatSamplesBelow(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, OTV0P2BASE::getByHourStat(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, OTV0P2BASE::STATS_SPECIAL_HOUR_NEXT_HOUR));
    const bool notLikelyOccupiedSoon = longLongVacant ||
        (likelyVacantNow &&
        // No more than about half the hours to be less occupied than this hour to be considered unlikely to be occupied.
        (hoursLessOccupiedThanThis < thisHourNLOThreshold) &&
        // Allow to be a little bit more occupied for the next hour than the current hour.
        // Suppress occupancy lookahead if room has been dark for several hours, eg overnight.  (TODO-792)
        (darkForHours || (hoursLessOccupiedThanNext < (thisHourNLOThreshold+1))));
    const uint8_t minLightsOffForSetbackMins = ecoBias ? 10 : 20;
    if(longVacant ||
       ((notLikelyOccupiedSoon || (dm > minLightsOffForSetbackMins) || (ecoBias && (Occupancy.getVacancyH() > 0) && (0 == OTV0P2BASE::getByHourStat(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR, OTV0P2BASE::STATS_SPECIAL_HOUR_CURRENT_HOUR)))) &&
           !Scheduler.isAnyScheduleOnWARMNow() && !recentUIControlUse()))
      {
      // Use a default minimal non-annoying setback if:
      //   in upper part of comfort range
      //   or if the room is likely occupied now
      //   or if the room is not known to be dark and hasn't been vacant for a long time ie ~1d and not in the very bottom range occupancy (TODO-107, TODO-758)
      //      TODO POSSIBLY: limit to (say) 3--4h light time for when someone out but room daylit, but note that detecting occupancy will be harder too in daylight.
      //      TODO POSSIBLY: after ~3h vacancy AND apparent smoothed occupancy non-zero (so some can be detected) AND ambient light in top quartile or in middle of typical bright part of cycle (assume peak of daylight) then being lit is not enough to prevent a deeper setback.
      //   or is fairly likely to be occupied in the next hour (to pre-warm) and the room hasn't been dark for hours and vacant for a long time
      //   or if a scheduled WARM period is due soon and the room hasn't been vacant for a long time,
      // else usually use a somewhat bigger 'eco' setback
      // else use an even bigger 'full' setback for maximum savings if in the eco region and
      //   the room has been vacant for a very long time
      //   or is unlikely to be unoccupied at this time of day and
      //     has been vacant and dark for a while or is in the lower part of the 'eco' range.
      // This final dark/vacant timeout to enter FULL fallback while in mild eco mode
      // should probably be longer than required to watch a typical movie or go to sleep (~2h) for example,
      // but short enough to take effect overnight and to be in effect a reasonable fraction of a (~8h) night.
      const uint8_t minVacantAndDarkForFULLSetbackH = 2; // Hours; strictly positive, typically 1--4.
      const uint8_t setback = (isComfortTemperature(wt) ||
                               Occupancy.isLikelyOccupied() ||
                               (!longVacant && !AmbLight.isRoomDark() && (hoursLessOccupiedThanThis > 4)) ||
                               (!longVacant && !darkForHours && (hoursLessOccupiedThanNext >= thisHourNLOThreshold-1)) ||
                               (!longVacant && Scheduler.isAnyScheduleOnWARMSoon())) ?
              SETBACK_DEFAULT :
          ((ecoBias && (longLongVacant ||
              (notLikelyOccupiedSoon && (isEcoTemperature(wt) ||
                  ((dm > (uint8_t)min(254, 60*minVacantAndDarkForFULLSetbackH)) && (Occupancy.getVacancyH() >= minVacantAndDarkForFULLSetbackH)))))) ?
              SETBACK_FULL : SETBACK_ECO);

      // Target must never be set low enough to create a frost/freeze hazard.
      const uint8_t newTarget = OTV0P2BASE::fnmax((uint8_t)(wt - setback), getFROSTTargetC());

      return(newTarget);
      }
    // Else use WARM target as-is.
    return(wt);
    }
  }


// Compute/update target temperature and set up state for tick()/computeRequiredTRVPercentOpen().
//
// Will clear any BAKE mode if the newly-computed target temperature is already exceeded.
void ModelledRadValve::computeTargetTemperature()
  {
  // Compute basic target temperature statelessly.
  const uint8_t newTarget = computeTargetTemp();

  // Explicitly compute the actual setback when in WARM mode for monitoring purposes.
  // TODO: also consider showing full setback to FROST when a schedule is set but not on.
  // By default, the setback is regarded as zero/off.
  setbackC = 0;
  if(inWarmMode())
    {
    const uint8_t wt = getWARMTargetC();
    if(newTarget < wt) { setbackC = wt - newTarget; }
    }

  // Set up state for computeRequiredTRVPercentOpen().
  inputState.targetTempC = newTarget;
  inputState.minPCOpen = getMinPercentOpen();
  inputState.maxPCOpen = getMaxPercentageOpenAllowed();
  inputState.glacial = glacial;
  inputState.inBakeMode = inBakeMode();
  inputState.hasEcoBias = hasEcoBias();
  // Request a fast response from the valve if user is manually adjusting controls.
  const bool veryRecentUIUse = veryRecentUIControlUse();
  inputState.fastResponseRequired = veryRecentUIUse;
  // Widen the allowed deadband significantly in an unlit/quiet/vacant room (TODO-383, TODO-593, TODO-786)
  // (or in FROST mode, or if temperature is jittery eg changing fast and filtering has been engaged)
  // to attempt to reduce the total number and size of adjustments and thus reduce noise/disturbance (and battery drain).
  // The wider deadband (less good temperature regulation) might be noticeable/annoying to sensitive occupants.
  // With a wider deadband may also simply suppress any movement/noise on some/most minutes while close to target temperature.
  // For responsiveness, don't widen the deadband immediately after manual controls have been used (TODO-593).
  //
  // Minimum number of hours vacant to force wider deadband in ECO mode, else a full day ('long vacant') is the threshold.
  // May still have to back this off if only automatic occupancy input is ambient light and day >> 6h, ie other than deep winter.
  const uint8_t minVacancyHoursForWideningECO = 3;
  inputState.widenDeadband = (!veryRecentUIUse) &&
      (retainedState.isFiltering ||
      (!inWarmMode()) ||
      AmbLight.isRoomDark() || // Must be false if light sensor not usable.
      Occupancy.longVacant() || (hasEcoBias() && (Occupancy.getVacancyH() >= minVacancyHoursForWideningECO)));
  // Capture adjusted reference/room temperatures
  // and set callingForHeat flag also using same outline logic as computeRequiredTRVPercentOpen() will use.
  inputState.setReferenceTemperatures(TemperatureC16.get());
  // True if the target temperature has not been met.
  const bool targetNotReached = (newTarget >= (inputState.refTempC16 >> 4));
  underTarget = targetNotReached;
  // If the target temperature is already reached then cancel any BAKE mode in progress (TODO-648).
  if(!targetNotReached) { cancelBakeDebounced(); }
  // Only report as calling for heat when actively doing so.
  // (Eg opening the valve a little in case the boiler is already running does not count.)
  callingForHeat = targetNotReached &&
    (value >= OTRadValve::DEFAULT_VALVE_PC_SAFER_OPEN) &&
    isControlledValveReallyOpen();
  }

// Compute target temperature and set heat demand for TRV and boiler; update state.
// CALL REGULARLY APPROXIMATELY ONCE PER MINUTE TO ALLOW SIMPLE TIME-BASED CONTROLS.
// Inputs are inWarmMode(), isRoomLit().
// The inputs must be valid (and recent).
// Values set are targetTempC, value (TRVPercentOpen).
// This may also prepare data such as TX command sequences for the TRV, boiler, etc.
// This routine may take significant CPU time; no I/O is done, only internal state is updated.
// Returns true if valve target changed and thus messages may need to be recomputed/sent/etc.
void ModelledRadValve::computeCallForHeat()
  {
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    // Run down BAKE mode timer if need be, one tick per minute.
    if(bakeCountdownM > 0) { --bakeCountdownM; }
    }

  // Compute target and ensure that required input state is set for computeRequiredTRVPercentOpen().
  computeTargetTemperature();
  retainedState.tick(value, inputState);
  }
//#endif // ENABLE_MODELLED_RAD_VALVE
#endif


// The STATS_SMOOTH_SHIFT is chosen to retain some reasonable precision within a byte and smooth over a weekly cycle.
#define STATS_SMOOTH_SHIFT 3 // Number of bits of shift for smoothed value: larger => larger time-constant; strictly positive.

// If defined, limit to stats sampling to one pre-sample and the final sample, to simplify/speed code.
#define STATS_MAX_2_SAMPLES

// Compute new linearly-smoothed value given old smoothed value and new value.
// Guaranteed not to produce a value higher than the max of the old smoothed value and the new value.
// Uses stochastic rounding to nearest to allow nominally sub-lsb values to have an effect over time.
// Usually only made public for unit testing.
uint8_t smoothStatsValue(const uint8_t oldSmoothed, const uint8_t newValue)
  {
  if(oldSmoothed == newValue) { return(oldSmoothed); } // Optimisation: smoothed value is unchanged if new value is the same as extant.
  // Compute and update with new stochastically-rounded exponentially-smoothed ("Brown's simple exponential smoothing") value.
  // Stochastic rounding allows sub-lsb values to have an effect over time.
  const uint8_t stocAdd = OTV0P2BASE::randRNG8() & ((1 << STATS_SMOOTH_SHIFT) - 1); // Allows sub-lsb values to have an effect over time.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("stocAdd=");
  DEBUG_SERIAL_PRINT(stocAdd);
  DEBUG_SERIAL_PRINTLN();
#endif
  // Do arithmetic in 16 bits to avoid over-/under- flows.
  return((uint8_t) (((((uint16_t) oldSmoothed) << STATS_SMOOTH_SHIFT) - ((uint16_t)oldSmoothed) + ((uint16_t)newValue) + stocAdd) >> STATS_SMOOTH_SHIFT));
  }

// Do an efficient division of an int total by small positive count to give a uint8_t mean.
//  * total running total, no higher than 255*sampleCount
//  * sampleCount small (<128) strictly positive number
static uint8_t smartDivToU8(const uint16_t total, const uint8_t sampleCount)
  {
#if 0 && defined(DEBUG) // Extra arg validation during dev.
  if(0 == sampleCount) { panic(); }
#endif
  if(1 == sampleCount) { return((uint8_t) total); } // No division required.
#if !defined(STATS_MAX_2_SAMPLES)
  // Generic divide (slow).
  if(2 != sampleCount) { return((uint8_t) ((total + (sampleCount>>1)) / sampleCount)); }
#elif 0 && defined(DEBUG)
  if(2 != sampleCount) { panic(); }
#endif
  // 2 samples.
  return((uint8_t) ((total+1) >> 1)); // Fast shift for 2 samples instead of slow divide.
  }

// Do simple update of last and smoothed stats numeric values.
// This assumes that the 'last' set is followed by the smoothed set.
// This autodetects unset values in the smoothed set and replaces them completely.
//   * lastSetPtr  is the offset in EEPROM of the 'last' value, with 'smoothed' assumed to be 24 bytes later.
//   * value  new stats value in range [0,254]
static void simpleUpdateStatsPair_(uint8_t * const lastEEPtr, const uint8_t value)
  {
#if 0 && defined(DEBUG) // Extra arg validation during dev.
  if((((int)lastEEPtr) < EE_START_STATS) || (((int)lastEEPtr)+24 > EE_END_STATS)) { panic(); }
  if(0xff == value) { panic(); }
#endif
  // Update the last-sample slot using the mean samples value.
  OTV0P2BASE::eeprom_smart_update_byte(lastEEPtr, value);
  // If existing smoothed value unset or invalid, use new one as is, else fold in.
  uint8_t * const pS = lastEEPtr + 24;
  const uint8_t smoothed = eeprom_read_byte(pS);
  if(0xff == smoothed) { OTV0P2BASE::eeprom_smart_update_byte(pS, value); }
  else { OTV0P2BASE::eeprom_smart_update_byte(pS, smoothStatsValue(smoothed, value)); }
  }
// Get some constant calculation done at compile time,
//   * lastSetN  is the set number for the 'last' values, with 'smoothed' assumed to be the next set.
//   * hh  hour for these stats [0,23].
//   * value  new stats value in range [0,254].
static inline void simpleUpdateStatsPair(const uint8_t lastSetN, const uint8_t hh, const uint8_t value)
  {
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINT_FLASHSTRING("stats update for set ");
    DEBUG_SERIAL_PRINT(lastSetN);
    DEBUG_SERIAL_PRINT_FLASHSTRING(" @");
    DEBUG_SERIAL_PRINT(hh);
    DEBUG_SERIAL_PRINT_FLASHSTRING("h = ");
    DEBUG_SERIAL_PRINT(value);
    DEBUG_SERIAL_PRINTLN();
#endif
  simpleUpdateStatsPair_((uint8_t *)(V0P2BASE_EE_STATS_START_ADDR(lastSetN) + (hh)), (value));
  }

// Sample statistics once per hour as background to simple monitoring and adaptive behaviour.
// Call this once per hour with fullSample==true, as near the end of the hour as possible;
// this will update the non-volatile stats record for the current hour.
// Optionally call this at a small (2--10) even number of evenly-spaced number of other times thoughout the hour
// with fullSample=false to sub-sample (and these may receive lower weighting or be ignored).
// (EEPROM wear should not be an issue at this update rate in normal use.)
void sampleStats(const bool fullSample)
  {
  // (Sub-)sample processing.
  // In general, keep running total of sub-samples in a way that should not overflow
  // and use the mean to update the non-volatile EEPROM values on the fullSample call.
  static uint8_t sampleCount_; // General sub-sample count; initially zero after boot, and zeroed after each full sample.
#if defined(STATS_MAX_2_SAMPLES)
  // Ensure maximum of two samples used: optional non-full sample then full/final one.
  if(!fullSample && (sampleCount_ != 0)) { return; }
#endif
  const bool firstSample = (0 == sampleCount_++);
#if defined(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK)
  // WARM mode count.
  static int8_t warmCount; // Sub-sample WARM count; initially zero, and zeroed after each full sample.
  if(inWarmMode()) { ++warmCount; } else { --warmCount; }
#endif
#if defined(ENABLE_AMBLIGHT_SENSOR)
  // Ambient light.
  const uint16_t ambLight = OTV0P2BASE::fnmin(AmbLight.get(), OTV0P2BASE::MAX_STATS_AMBLIGHT); // Constrain value at top end to avoid 'not set' value.
  static uint16_t ambLightTotal;
  ambLightTotal = firstSample ? ambLight : (ambLightTotal + ambLight);
#endif
  const int tempC16 = TemperatureC16.get();
  static int tempC16Total;
  tempC16Total = firstSample ? tempC16 : (tempC16Total + tempC16);
#if defined(ENABLE_OCCUPANCY_SUPPORT)
  const uint16_t occpc = Occupancy.get();
  static uint16_t occpcTotal;
  occpcTotal = firstSample ? occpc : (occpcTotal + occpc);
#endif
#if defined(HUMIDITY_SENSOR_SUPPORT)
  // Assume for now RH% always available (compile-time determined) or not; not intermittent.
  // TODO: allow this to work with at least start-up-time availability detection.
  const uint16_t rhpc = OTV0P2BASE::fnmin(RelHumidity.get(), (uint8_t)100); // Fail safe.
  static uint16_t rhpcTotal;
  rhpcTotal = firstSample ? rhpc : (rhpcTotal + rhpc);
#endif
  if(!fullSample) { return; } // Only accumulate values cached until a full sample.
  // Catpure sample count to use below.
  const uint8_t sc = sampleCount_; 
  // Reset generic sub-sample count to initial state after fill sample.
  sampleCount_ = 0;

  // Get the current local-time hour...
  const uint_least8_t hh = OTV0P2BASE::getHoursLT(); 

  // Scale and constrain last-read temperature to valid range for stats.
#if defined(STATS_MAX_2_SAMPLES)
  const int tempCTotal = (1==sc)?tempC16Total:((tempC16Total+1)>>1);
#else
  const int tempCTotal = (1==sc)?tempC16Total:
                         ((2==sc)?((tempC16Total+1)>>1):
                                  ((tempC16Total + (sc>>1)) / sc));
#endif
  const uint8_t temp = OTV0P2BASE::compressTempC16(tempCTotal);
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("SU tempC16Total=");
  DEBUG_SERIAL_PRINT(tempC16Total);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", tempCTotal=");
  DEBUG_SERIAL_PRINT(tempCTotal);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", temp=");
  DEBUG_SERIAL_PRINT(temp);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", expanded=");
  DEBUG_SERIAL_PRINT(expandTempC16(temp));
  DEBUG_SERIAL_PRINTLN();
#endif
  simpleUpdateStatsPair(V0P2BASE_EE_STATS_SET_TEMP_BY_HOUR, hh, temp);

#if defined(ENABLE_AMBLIGHT_SENSOR)
  // Ambient light; last and smoothed data sets,
  simpleUpdateStatsPair(V0P2BASE_EE_STATS_SET_AMBLIGHT_BY_HOUR, hh, smartDivToU8(ambLightTotal, sc));
#endif

#if defined(ENABLE_OCCUPANCY_SUPPORT)
  // Occupancy confidence percent, if supported; last and smoothed data sets,
  simpleUpdateStatsPair(V0P2BASE_EE_STATS_SET_OCCPC_BY_HOUR, hh, smartDivToU8(occpcTotal, sc));
#endif 

#if defined(HUMIDITY_SENSOR_SUPPORT)
  // Relative humidity percent, if supported; last and smoothed data sets,
  simpleUpdateStatsPair(V0P2BASE_EE_STATS_SET_RHPC_BY_HOUR, hh, smartDivToU8(rhpcTotal, sc));
#endif

#if defined(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK)
  // Update sampled WARM-mode value.
  // 0xff when unset/erased; first use will set all history bits to the initial sample value.
  // When in use, bit 7 (msb) is always 0 (to distinguish from unset).
  // Bit 6 is 1 if most recent day's sample was in WARM (or BAKE) mode, 0 if in FROST mode.
  // At each new sampling, bits 6--1 are shifted down and the new bit 6 set as above.
  // Designed to enable low-wear no-write or selective erase/write use much of the time;
  // periods which are always the same mode will achieve a steady-state value (eliminating most EEPROM wear)
  // while even some of the rest (while switching over from all-WARM to all-FROST) will only need pure writes (no erase).
  uint8_t *const phW = (uint8_t *)(V0P2BASE_EE_STATS_START_ADDR(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK) + hh);
  const uint8_t warmHistory = eeprom_read_byte(phW);
  if(warmHistory & 0x80) { eeprom_smart_clear_bits(phW, inWarmMode() ? 0x7f : 0); } // First use sets all history bits to current sample value.
  else // Shift in today's sample bit value for this hour at bit 6...
    {
    uint8_t newWarmHistory = (warmHistory >> 1) & 0x3f;
    if(warmCount > 0) { newWarmHistory |= 0x40; } // Treat as warm iff more WARM than FROST (sub-)samples.
    eeprom_smart_update_byte(phW, newWarmHistory);
    }
  // Reset WARM sub-sample count after full sample.
  warmCount = 0;
#endif

  // TODO: other stats measures...
  }


#ifdef ENABLE_FS20_ENCODING_SUPPORT
// Clear and populate core stats structure with information from this node.
// Exactly what gets filled in will depend on sensors on the node,
// and may depend on stats TX security level (eg if collecting some sensitive items is also expensive).
void populateCoreStats(OTV0P2BASE::FullStatsMessageCore_t *const content)
  {
  clearFullStatsMessageCore(content); // Defensive programming: all fields should be set explicitly below.
  if(localFHT8VTRVEnabled())
    {
    // Use FHT8V house codes if available.
    content->id0 = FHT8V.nvGetHC1();
    content->id1 = FHT8V.nvGetHC2();
    }
  else
    {
    // Use OpenTRV unique ID if no other higher-priority ID.
    content->id0 = eeprom_read_byte(0 + (uint8_t *)V0P2BASE_EE_START_ID);
    content->id1 = eeprom_read_byte(1 + (uint8_t *)V0P2BASE_EE_START_ID);
    }
  content->containsID = true;
  content->tempAndPower.tempC16 = TemperatureC16.get();
  content->tempAndPower.powerLow = Supply_cV.isSupplyVoltageLow();
  content->containsTempAndPower = true;
  content->ambL = OTV0P2BASE::fnmax((uint8_t)1, OTV0P2BASE::fnmin((uint8_t)254, AmbLight.get())); // Coerce to allowed value in range [1,254]. Bug-fix (twice! TODO-510) c/o Gary Gladman!
  content->containsAmbL = true;
  // OC1/OC2 = Occupancy: 00 not disclosed, 01 not occupied, 10 possibly occupied, 11 probably occupied.
  // The encodeFullStatsMessageCore() route should omit data not appopriate for security reasons.
#ifdef ENABLE_OCCUPANCY_SUPPORT
  content->occ = Occupancy.twoBitOccupancyValue();
#else
  content->occ = 0; // Not supported.
#endif
  }
#endif // ENABLE_FS20_ENCODING_SUPPORT


// Call this to do an I/O poll if needed; returns true if something useful definitely happened.
// This call should typically take << 1ms at 1MHz CPU.
// Does not change CPU clock speeds, mess with interrupts (other than possible brief blocking), or sleep.
// Should also do nothing that interacts with Serial.
// Limits actual poll rate to something like once every 8ms, unless force is true.
//   * force if true then force full poll on every call (ie do not internally rate-limit)
// Note that radio poll() can be for TX as well as RX activity.
// Not thread-safe, eg not to be called from within an ISR.
bool pollIO(const bool force)
  {
#ifdef ENABLE_RADIO_PRIMARY_MODULE
  static volatile uint8_t _pO_lastPoll;
  // Poll RX at most about every ~8ms.
  const uint8_t sct = OTV0P2BASE::getSubCycleTime();
  if(force || (sct != _pO_lastPoll))
    {
    _pO_lastPoll = sct;
    // Poll for inbound frames.
    // If RX is not interrupt-driven then
    // there will usually be little time to do this
    // before getting an RX overrun or dropped frame.
    PrimaryRadio.poll();
  #ifdef ENABLE_RADIO_SECONDARY_MODULE
    SecondaryRadio.poll();
  #endif
    }
#endif
  return(false);
  }

#ifdef ENABLE_STATS_TX
#if defined(ENABLE_JSON_OUTPUT)
// Managed JSON stats.
static OTV0P2BASE::SimpleStatsRotation<10> ss1; // Configured for maximum different stats.	// FIXME increased for voice
#endif // ENABLE_STATS_TX
// Do bare stats transmission.
// Output should be filtered for items appropriate
// to current channel security and sensitivity level.
// This may be binary or JSON format.
//   * allowDoubleTX  allow double TX to increase chance of successful reception
//   * doBinary  send binary form if supported, else JSON form if supported
// Sends stats on primary radio channel 0 with possible duplicate to secondary channel.
// If sending encrypted then ID/counter fields (eg @ and + for JSON) are omitted
// as assumed supplied by security layer to remote recipent.
void bareStatsTX(const bool allowDoubleTX, const bool doBinary)
  {
  // Note if radio/comms channel is itself framed.
  const bool framed = !PrimaryRadio.getChannelConfig()->isUnframed;
#if defined(ENABLE_RFM23B_FS20_RAW_PREAMBLE)
  // Add RFM23B preamble and a trailing CRC to the frame IFF channel is unframed.
  const bool RFM23BFramed = !framed;
#else
  const bool RFM23BFramed = false; // Never use this raw framing unless enabled explicitly.
#endif

#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
  const bool doEnc = true;
#else
  const bool doEnc = false;
#endif

  const bool neededWaking = OTV0P2BASE::powerUpSerialIfDisabled<V0P2_UART_BAUD>(); // FIXME
#if (FullStatsMessageCore_MAX_BYTES_ON_WIRE > STATS_MSG_MAX_LEN)
#error FullStatsMessageCore_MAX_BYTES_ON_WIRE too big
#endif // FullStatsMessageCore_MAX_BYTES_ON_WIRE > STATS_MSG_MAX_LEN
#if (MSG_JSON_MAX_LENGTH+1 > STATS_MSG_MAX_LEN) // Allow 1 for trailing CRC.
#error MSG_JSON_MAX_LENGTH too big
#endif // MSG_JSON_MAX_LENGTH+1 > STATS_MSG_MAX_LEN

  // Allow space in buffer for:
  //   * buffer offset/preamble
  //   * max binary length, or max JSON length + 1 for CRC + 1 to allow detection of oversize message
  //   * terminating 0xff
//  uint8_t buf[STATS_MSG_START_OFFSET + max(FullStatsMessageCore_MAX_BYTES_ON_WIRE,  MSG_JSON_MAX_LENGTH+1) + 1];
  // Buffer need be no larger than leading length byte + typical 64-byte radio module TX buffer limit + optional terminator.
  const uint8_t MSG_BUF_SIZE = 1 + 64 + 1;
  uint8_t buf[MSG_BUF_SIZE];
#if 0
  // Make sure buffer is cleared for debug purposes
  memset(buf, 0, sizeof(buf));
#endif // 0

#if defined(ENABLE_JSON_OUTPUT)
  if(doBinary && !doEnc) // Note that binary form is not secure, so not permitted for secure systems.
#endif // ENABLE_JSON_OUTPUT
    {
#if defined(ENABLE_BINARY_STATS_TX) && defined(ENABLE_FS20_ENCODING_SUPPORT) && !defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
    // Send binary message first (insecure, FS20-piggyback format).
    // Gather core stats.
    OTV0P2BASE::FullStatsMessageCore_t content;
    populateCoreStats(&content);
    const uint8_t *msg1 = encodeFullStatsMessageCore(buf + STATS_MSG_START_OFFSET, sizeof(buf) - STATS_MSG_START_OFFSET, OTV0P2BASE::getStatsTXLevel(), false, &content);
    if(NULL == msg1)
      {
#if 0
DEBUG_SERIAL_PRINTLN_FLASHSTRING("Bin gen err!");
#endif
      return;
      }
    // Send it!
    RFM22RawStatsTXFFTerminated(buf, allowDoubleTX);
    // Record stats as if remote, and treat channel as secure.
    outputCoreStats(&Serial, true, &content);
    handleQueuedMessages(&Serial, false, &PrimaryRadio); // Serial must already be running!
#endif // defined(ENABLE_BINARY_STATS_TX) ...
    }

#if defined(ENABLE_JSON_OUTPUT)
  else // Send binary *or* JSON on each attempt so as not to overwhelm the receiver.
    {
    // Send JSON message.
    bool sendingJSONFailed = false; // Set true and stop attempting JSON send in case of error.

    // Set pointer location based on whether start of message will have preamble TODO move to OTRFM23BLink queueToSend?
    uint8_t *bptr = buf;
    if(RFM23BFramed) { bptr += STATS_MSG_START_OFFSET; }
    // Leave space for possible leading frame-length byte, eg for encrypted frame.
    else { ++bptr; }
    // Where to write the real frame content.
    uint8_t *const realTXFrameStart = bptr;

    // If forcing encryption or if unconditionally suppressed
    // then suppress the "@" ID field entirely,
    // assuming that the encrypted commands will carry the ID, ie in the 'envelope'.
#if defined(ENABLE_JSON_SUPPRESSED_ID)
    if(true)
#else
    if(doEnc)
#endif // defined(ENABLE_JSON_SUPPRESSED_ID)
        { static const char nul[1] = {}; ss1.setID(nul); }
    else
      {
#if defined(ENABLE_FHT8VSIMPLE)
      // Insert FHT8V-style ID in stats messages if appropriate.
      // Will not be appropriate if primary channel provides ID itself.
      static char idBuf[5]; // Static so as to have lifetime no shorter than ss1.
      if(localFHT8VTRVEnabled())
        {
        const uint8_t hc1 = FHT8V.nvGetHC1();
        const uint8_t hc2 = FHT8V.nvGetHC2();
        idBuf[0] = OTV0P2BASE::hexDigit(hc1 >> 4);
        idBuf[1] = OTV0P2BASE::hexDigit(hc1);
        idBuf[2] = OTV0P2BASE::hexDigit(hc2 >> 4);
        idBuf[3] = OTV0P2BASE::hexDigit(hc2);
        idBuf[4] = '\0';
        ss1.setID(idBuf);
        }
      else { ss1.setID(NULL); } // Use built-in ID.
#endif
      }

    // Managed JSON stats.
#if defined(ENABLE_JSON_FRAME_MINIMISED)
    // Minimise frame size (eg for noisy radio links)...
    const bool maximise = false;
    // Suppress "+" count field, accepting loss of diagnostics.
    ss1.enableCount(false); 
#else
    // Make best use of available bandwidth...
    const bool maximise = true;
    // Enable "+" count field for diagnostic purposes, eg while TX is lossy,
    // if the primary radio channel does not include a sequence number itself.
    // Assume that an encrypted channel will provide its own (visible) sequence counter.
    ss1.enableCount(!doEnc); 
#endif // defined(ENABLE_JSON_FRAME_MINIMISED)
//    if(ss1.isEmpty())
//      {
//      // Perform run-once operations...
//      }
    ss1.put(TemperatureC16);
#if defined(HUMIDITY_SENSOR_SUPPORT)
    ss1.put(RelHumidity);
#endif // defined(HUMIDITY_SENSOR_SUPPORT)
#if defined(ENABLE_OCCUPANCY_SUPPORT)
    ss1.put(Occupancy.twoBitTag(), Occupancy.twoBitOccupancyValue()); // Reduce spurious TX cf percentage.
#if !defined(ENABLE_TRIMMED_BANDWIDTH)
    ss1.put(Occupancy.vacHTag(), Occupancy.getVacancyH(), true); // Low priority as notionally redundant.
#endif // !defined(ENABLE_TRIMMED_BANDWIDTH)
#endif // defined(ENABLE_OCCUPANCY_SUPPORT)
    // OPTIONAL items
    // Only TX supply voltage for units apparently not mains powered, and TX with low priority as slow changing.
    if(!Supply_cV.isMains()) { ss1.put(Supply_cV, true); } else { ss1.remove(Supply_cV.tag()); }
#ifdef ENABLE_BOILER_HUB
    // Show boiler state for boiler hubs.
    ss1.put("b", (int) isBoilerOn());
#endif // ENABLE_BOILER_HUB
#ifdef ENABLE_AMBLIGHT_SENSOR
    ss1.put(AmbLight); // Always send ambient light level (assuming sensor is present).
#endif // ENABLE_AMBLIGHT_SENSOR
#ifdef ENABLE_VOICE_STATS
    ss1.put(Voice);
#endif // ENABLE_VOICE_STATS
#if defined(ENABLE_LOCAL_TRV)
    // Show TRV-related stats since enabled.
    ss1.put(NominalRadValve);
    ss1.put(NominalRadValve.tagTTC(), NominalRadValve.getTargetTempC());
    ss1.put(NominalRadValve.tagTSC(), NominalRadValve.getSetbackC(), true); // Low priority; depth matters more than speed.
#if !defined(ENABLE_TRIMMED_BANDWIDTH)
    ss1.put(NominalRadValve.tagCMPC(), NominalRadValve.getCumulativeMovementPC(), true); // Low priority as notionally redundant.
#endif // !defined(ENABLE_TRIMMED_BANDWIDTH)
#endif // defined(ENABLE_LOCAL_TRV)

#if defined(ENABLE_ALWAYS_TX_ALL_STATS)
    const uint8_t privacyLevel = OTV0P2BASE::stTXalwaysAll;
#else
    const uint8_t privacyLevel = OTV0P2BASE::getStatsTXLevel();
#endif

    // Buffer to write JSON to before encryption.
    // Size for JSON in 'O' frame is:
    //    ENC_BODY_SMALL_FIXED_PTEXT_MAX_SIZE - 2 leading body bytes + for trailing '}' not sent.
    const uint8_t maxSecureJSONSize = OTRadioLink::ENC_BODY_SMALL_FIXED_PTEXT_MAX_SIZE - 2 + 1;
    // writeJSON() requires two further bytes including one for the trailing '\0'.
    uint8_t ptextBuf[maxSecureJSONSize + 2];

    // Allow for a cap on JSON TX size, eg where TX is lossy for near-maximum sizes.
    // This can only reduce the maximum size, and it should not try to make it silly small.
#if defined(ENABLE_JSON_STATS_LEN_CAP)
    static const uint8_t max_plaintext_JSON_len = min(OTV0P2BASE::MSG_JSON_MAX_LENGTH, ENABLE_JSON_STATS_LEN_CAP);
#else
    static const uint8_t max_plaintext_JSON_len = OTV0P2BASE::MSG_JSON_MAX_LENGTH;
#endif

    // Redirect JSON output appropriately.
    uint8_t *const bufJSON = doEnc ? ptextBuf : bptr;
    const uint8_t bufJSONlen = doEnc ? sizeof(ptextBuf) : min(max_plaintext_JSON_len+2, sizeof(buf) - (bptr-buf));

    // Number of bytes written for body.
    // For non-secure, this is the size of the JSON text.
    // For secure this is overridden with the secure frame size.
    int8_t wrote = 0;

    // Generate JSON text.
    if(!sendingJSONFailed)
      {
      // Generate JSON and write to appropriate buffer:
      // direct to TX buffer if not encrypting, else to separate buffer.
      wrote = ss1.writeJSON(bufJSON, bufJSONlen, privacyLevel, maximise); //!allowDoubleTX && randRNG8NextBoolean());
      if(0 == wrote)
        {
#if 0 && defined(DEBUG)
DEBUG_SERIAL_PRINTLN_FLASHSTRING("JSON gen err!");
#endif
        sendingJSONFailed = true;
        }
      }

    // Push the JSON output to Serial.
    if(!sendingJSONFailed)
      {
 #if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      if(doEnc)
        {
        // Insert synthetic full ID/@ field for local stats, but no sequence number for now.
        Serial.print(F("{\"@\":\""));
        for(int i = 0; i < OTV0P2BASE::OpenTRV_Node_ID_Bytes; ++i) { Serial.print(eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_ID+i), HEX); }
        Serial.print(F("\","));
        Serial.write(bufJSON+1, wrote-1);
        Serial.println();
        }
      else
#endif // defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
        { OTV0P2BASE::outputJSONStats(&Serial, true, bufJSON, bufJSONlen); } // Serial must already be running!
      OTV0P2BASE::flushSerialSCTSensitive(); // Ensure all flushed since system clock may be messed with...
      }

    // Get the 'building' key for stats sending.
    uint8_t key[16];
    if(!sendingJSONFailed && doEnc)
      {
#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      if(!OTV0P2BASE::getPrimaryBuilding16ByteSecretKey(key))
        {
        sendingJSONFailed = true;
#if 1 // && defined(DEBUG)
        OTV0P2BASE::serialPrintlnAndFlush(F("!TX key")); // Know why TX failed.
#endif
        }
#else
      sendingJSONFailed = true; // Crypto support may not be available.
#endif // defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      }

    // If doing encryption
    // then build encrypted frame from raw JSON.
    if(!sendingJSONFailed && doEnc)
      {
#if defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      const OTRadioLink::SimpleSecureFrame32or0BodyTXBase::fixed32BTextSize12BNonce16BTagSimpleEnc_ptr_t e = OTAESGCM::fixed32BTextSize12BNonce16BTagSimpleEnc_DEFAULT_STATELESS;
      const uint8_t txIDLen = OTRadioLink::ENC_BODY_DEFAULT_ID_BYTES;
      // When sending on a channel with framing, do not explicitly send the frame length byte.
      const uint8_t offset = framed ? 1 : 0;
      // Assumed to be at least one free writeable byte ahead of bptr.
#if defined(ENABLE_NOMINAL_RAD_VALVE)
      // Get current modelled valve position.
      const uint8_t valvePC = NominalRadValve.get();
#else
      // Distinguished 'invalid' valve position; never mistaken for a real valve.
      const uint8_t valvePC = 0x7f;
#endif // defined(ENABLE_NOMINAL_RAD_VALVE)
      const uint8_t bodylen = OTRadioLink::SimpleSecureFrame32or0BodyTXV0p2::getInstance().generateSecureOFrameRawForTX(
            realTXFrameStart - offset, sizeof(buf) - (realTXFrameStart-buf) + offset,
            txIDLen, valvePC, (const char *)bufJSON, e, NULL, key);
      sendingJSONFailed = (0 == bodylen);
      wrote = bodylen - offset;
#else
      sendingJSONFailed = true; // Crypto support may not be available.
#endif // defined(ENABLE_OTSECUREFRAME_ENCODING_SUPPORT)
      }

#if 0 && defined(DEBUG)
    if(sendingJSONFailed) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("!failed JSON enc"); }
#endif

#ifdef ENABLE_RADIO_SECONDARY_MODULE
    if(!sendingJSONFailed)
      {
      // Write out unadjusted JSON or encrypted frame on secondary radio.
//      SecondaryRadio.queueToSend(realTXFrameStart, doEnc ? (bptr - realTXFrameStart) : wrote);
      // Assumes that framing (or not) of primary and secondary radios is the same (usually: both framed).
      SecondaryRadio.queueToSend(realTXFrameStart, wrote);
      }
#endif // ENABLE_RADIO_SECONDARY_MODULE

#ifdef ENABLE_RADIO_RX
    handleQueuedMessages(&Serial, false, &PrimaryRadio); // Serial must already be running!
#endif

    if(!sendingJSONFailed)
      {
      // If not encrypting, adjust the JSON for transmission and add a CRC.
      // (Set high-bit on final closing brace to make it unique, and compute (non-0xff) CRC.)
      if(!doEnc)
          {
          const uint8_t crc = OTV0P2BASE::adjustJSONMsgForTXAndComputeCRC((char *)bptr);
          if(0xff == crc) { sendingJSONFailed = true; }
          else
            {
            bptr += wrote;
            *bptr++ = crc; // Add 7-bit CRC for on-the-wire check.
            ++wrote;
            }
          }

#if defined(ENABLE_RFM23B_FS20_RAW_PREAMBLE)
      // Use ugly 0xff-terminated RFM23B send.
      if(RFM23BFramed)
        {
        *bptr = 0xff; // Terminate message for TX.
        RFM22RawStatsTXFFTerminated(buf, allowDoubleTX, RFM23BFramed);
        }
      else
#endif
        {
        // Send directly to the primary radio...
        PrimaryRadio.queueToSend(realTXFrameStart, wrote);
        }
      }

#if 1 && defined(DEBUG)
    if(sendingJSONFailed) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("!failed JSON TX"); }
#endif
    }
#endif // defined(ENABLE_JSON_OUTPUT)

//DEBUG_SERIAL_PRINTLN_FLASHSTRING("Stats TX");
  if(neededWaking) { OTV0P2BASE::flushSerialProductive(); OTV0P2BASE::powerDownSerial(); }
  }
#endif // defined(ENABLE_STATS_TX)



// Wire components directly together, eg for occupancy sensing.
static void wireComponentsTogether()
  {
#ifdef ENABLE_FHT8VSIMPLE
  // Set up radio with FHT8V.
  FHT8V.setRadio(&PrimaryRadio);
  // Load EEPROM house codes into primary FHT8V instance at start.
  FHT8V.nvLoadHC();
#endif // ENABLE_FHT8VSIMPLE

#if defined(ENABLE_OCCUPANCY_SUPPORT) && defined(ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT)
  AmbLight.setPossOccCallback(genericMarkAsPossiblyOccupied);
#endif // ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT

#if defined(ENABLE_OCCUPANCY_SUPPORT) && defined(ENABLE_OCCUPANCY_DETECTION_FROM_VOICE)
  Voice.setPossOccCallback(genericMarkAsPossiblyOccupied);
#endif // ENABLE_OCCUPANCY_DETECTION_FROM_VOICE

#if defined(TEMP_POT_AVAILABLE)
//  TempPot.setOccCallback(genericMarkAsOccupied); // markUIControlUsed
  // Mark UI as used and indirectly mark occupancy when control is used.
  TempPot.setOccCallback(markUIControlUsed);
  // Callbacks to set various mode combinations.
  // Typically at most one call would be made on any appropriate pot adjustment.
  TempPot.setWFBCallbacks(setWarmModeFromManualUI, setBakeModeFromManualUI);
#endif // TEMP_POT_AVAILABLE

#if V0p2_REV == 14
  pinMode(REGULATOR_POWERUP, OUTPUT);
#ifdef ENABLE_VOICE_SENSOR
  fastDigitalWrite(REGULATOR_POWERUP, HIGH);
#else
  fastDigitalWrite(REGULATOR_POWERUP, LOW);
#endif // ENABLE_VOICE_SENSOR
#endif // V0p2_REV == 14
  }


// Initialise sensors with stats info where needed.
// Should be called at least hourly after all stats have been updatedß,
// but can be called whenever user adjusts settings for example.
static void updateSensorsFromStats()
  {
#if defined(ENABLE_AMBLIGHT_SENSOR) && defined(ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT)
  // Update with rolling stats to adapt to sensors and local environment.
  // ...and prevailing mode, so may take a while to adjust.
  AmbLight.setMinMax(
          OTV0P2BASE::getMinByHourStat(V0P2BASE_EE_STATS_SET_AMBLIGHT_BY_HOUR),
          OTV0P2BASE::getMaxByHourStat(V0P2BASE_EE_STATS_SET_AMBLIGHT_BY_HOUR),
          OTV0P2BASE::getMinByHourStat(V0P2BASE_EE_STATS_SET_AMBLIGHT_BY_HOUR_SMOOTHED),
          OTV0P2BASE::getMaxByHourStat(V0P2BASE_EE_STATS_SET_AMBLIGHT_BY_HOUR_SMOOTHED),
          !hasEcoBias());
#endif // ENABLE_OCCUPANCY_DETECTION_FROM_AMBLIGHT
  }

// Run tasks needed at the end of each hour.
// Should be run once at a fixed slot in the last minute of each hour.
// Will be run after all stats for the current hour have been updated.
static void endOfHourTasks()
  {
#if defined(ENABLE_SETBACK_LOCKOUT_COUNTDOWN)
    // Count down the lockout if not finished...  (TODO-786)
    const uint8_t sloInv = eeprom_read_byte((uint8_t *)OTV0P2BASE::V0P2BASE_EE_START_SETBACK_LOCKOUT_COUNTDOWN_H_INV);
    if(0xff != sloInv)
      {
      // Logically decrement the inverted value, invert it and store it back.
      const uint8_t updated = ~((~sloInv)-1);
      OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)OTV0P2BASE::V0P2BASE_EE_START_SETBACK_LOCKOUT_COUNTDOWN_H_INV, updated);
      }
#endif
  }


// Controller's view of Least Significant Digits of the current (local) time, in this case whole seconds.
// See PICAXE V0.1/V0.09/DHD201302L0 code.
#define TIME_LSD_IS_BINARY // TIME_LSD is in binary (cf BCD).
#define TIME_CYCLE_S 60 // TIME_LSD ranges from 0 to TIME_CYCLE_S-1, also major cycle length.
static uint_fast8_t TIME_LSD; // Controller's notion of seconds within major cycle.

// 'Elapsed minutes' count of minute/major cycles; cheaper than accessing RTC and not tied to real time.
// Starts at or just above zero (within the first 4-minute cycle) to help avoid collisions between units after mass power-up.
// Wraps at its maximum (0xff) value.
static uint8_t minuteCount;

// Mask for Port B input change interrupts.
#define MASK_PB_BASIC 0b00000000 // Nothing.
#if defined(PIN_RFM_NIRQ) && defined(ENABLE_RADIO_RX) // RFM23B IRQ only used for RX.
  #if (PIN_RFM_NIRQ < 8) || (PIN_RFM_NIRQ > 15)
    #error PIN_RFM_NIRQ expected to be on port B
  #endif
  #define RFM23B_INT_MASK (1 << (PIN_RFM_NIRQ&7))
  #define MASK_PB (MASK_PB_BASIC | RFM23B_INT_MASK)
#else
  #define MASK_PB MASK_PB_BASIC
#endif

// Mask for Port C input change interrupts.
#define MASK_PC_BASIC 0b00000000 // Nothing.

// Mask for Port D input change interrupts.
#define MASK_PD_BASIC 0b00000001 // Serial RX by default.
#if defined(ENABLE_VOICE_SENSOR)
  #if VOICE_NIRQ > 7
    #error VOICE_NIRQ expected to be on port D
  #endif
  #define VOICE_INT_MASK (1 << (VOICE_NIRQ&7))
  #define MASK_PD1 (MASK_PD_BASIC | VOICE_INT_MASK)
#else
  #define MASK_PD1 MASK_PD_BASIC // Just serial RX, no voice.
#endif
#if defined(ENABLE_SIMPLIFIED_MODE_BAKE)
#if BUTTON_MODE_L > 7
  #error BUTTON_MODE_L expected to be on port D
#endif
  #define MODE_INT_MASK (1 << (BUTTON_MODE_L&7))
  #define MASK_PD (MASK_PD1 | MODE_INT_MASK) // MODE button interrupt (et al).
#else
  #define MASK_PD MASK_PD1 // No MODE button interrupt.
#endif

void setupOpenTRV()
  {
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("Entering setup...");
#endif

  // Radio not listening to start with.
  // Ignore any initial spurious RX interrupts for example.
  PrimaryRadio.listen(false);

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("PrimaryRadio.listen(false);");
#endif

  // Set up async edge interrupts.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    //PCMSK0 = PB; PCINT  0--7    (LEARN1 and Radio)
    //PCMSK1 = PC; PCINT  8--15
    //PCMSK2 = PD; PCINT 16--24   (Serial RX and LEARN2 and MODE and Voice)

    PCICR =
#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
        1 | // 0x1 enables PB/PCMSK0.
#endif
#if defined(MASK_PC) && (MASK_PC != 0) // If PC interrupts required.
        2 | // 0x2 enables PC/PCMSK1.
#endif
#if defined(MASK_PD) && (MASK_PD != 0) // If PD interrupts required.
        4 | // 0x4 enables PD/PCMSK2.
#endif
        0;

#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
    PCMSK0 = MASK_PB;
#endif
#if defined(MASK_PC) && (MASK_PC != 0) // If PC interrupts required.
    PCMSK1 = MASK_PC;
#endif
#if defined(MASK_PD) && (MASK_PD != 0) // If PD interrupts required.
    PCMSK2 = MASK_PD;
#endif
    }

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("ints set up");
#endif

  // Wire components directly together, eg for occupancy sensing.
  wireComponentsTogether();

  // Initialise sensors with stats info where needed.
  updateSensorsFromStats();

#ifdef ENABLE_STATS_TX
  // Do early 'wake-up' stats transmission if possible
  // when everything else is set up and ready and allowed (TODO-636)
  // including all set-up and inter-wiring of sensors/actuators.
  if(enableTrailingStatsPayload())
    {
    // Attempt to maximise chance of reception with a double TX.
    // Assume not in hub mode (yet).
    // Send all possible formats, binary first (assumed complete in one message).
    bareStatsTX(true, true);
    // Send JSON stats repeatedly (typically once or twice)
    // until all values pushed out (no 'changed' values unsent)
    // or limit reached.
    for(uint8_t i = 5; --i > 0; )
      {
      ::OTV0P2BASE::nap(WDTO_120MS, false); // Sleep long enough for receiver to have a chance to process previous TX.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING(" TX...");
#endif
      bareStatsTX(true, false);
      if(!ss1.changedValue()) { break; }
      }
  //  nap(WDTO_120MS, false);
    }
#endif

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("setup stats sent");
#endif

#if !defined(DONT_RANDOMISE_MINUTE_CYCLE)
  // Start local counters in randomised positions to help avoid inter-unit collisions,
  // eg for mains-powered units starting up together after a power cut,
  // but without (eg) breaking any of the logic about what order things will be run first time through.
  // Uses some decent noise to try to start the units separated.
  const uint8_t b = OTV0P2BASE::getSecureRandomByte(); // randRNG8();
  // Start within bottom half of minute (or close to); sensor readings happen in second half.
  OTV0P2BASE::setSeconds(b >> 2);
  // Start anywhere in first 4 minute cycle.
  minuteCount = b & 3;
#endif

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("Finishing setup...");
#endif

#if 0
  // Provide feedback to user that UI is coming to life (if any).
  userOpFeedback();
#endif

  // Set appropriate loop() values just before entering it.
  TIME_LSD = OTV0P2BASE::getSecondsLT();
  }

#if !defined(ALT_MAIN_LOOP) // Do not define handlers here when alt main is in use.

#if defined(MASK_PB) && (MASK_PB != 0) // If PB interrupts required.
//// Interrupt count.  Marked volatile so safe to read without a lock as is a single byte.
//static volatile uint8_t intCountPB;
// Previous state of port B pins to help detect changes.
static volatile uint8_t prevStatePB;
// Interrupt service routine for PB I/O port transition changes.
ISR(PCINT0_vect)
  {
//  ++intCountPB;
  const uint8_t pins = PINB;
  const uint8_t changes = pins ^ prevStatePB;
  prevStatePB = pins;

#if defined(RFM23B_INT_MASK)
  // RFM23B nIRQ falling edge is of interest.
  // Handler routine not required/expected to 'clear' this interrupt.
  // TODO: try to ensure that OTRFM23BLink.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
  if((changes & RFM23B_INT_MASK) && !(pins & RFM23B_INT_MASK))
    { PrimaryRadio.handleInterruptSimple(); }
#endif
  }
#endif

#if defined(MASK_PC) && (MASK_PC != 0) // If PC interrupts required.
// Previous state of port C pins to help detect changes.
static volatile uint8_t prevStatePC;
// Interrupt service routine for PC I/O port transition changes.
ISR(PCINT1_vect)
  {
//  const uint8_t pins = PINC;
//  const uint8_t changes = pins ^ prevStatePC;
//  prevStatePC = pins;
//
// ...
  }
#endif

#if defined(MASK_PD) && (MASK_PD != 0) // If PD interrupts required.
// Previous state of port D pins to help detect changes.
static volatile uint8_t prevStatePD;
// Interrupt service routine for PD I/O port transition changes (including RX).
ISR(PCINT2_vect)
  {
  const uint8_t pins = PIND;
  const uint8_t changes = pins ^ prevStatePD;
  prevStatePD = pins;

#if defined(ENABLE_SIMPLIFIED_MODE_BAKE)
  // Mode button detection is on the falling edge (button pressed).
  if((changes & MODE_INT_MASK) && !(pins & MODE_INT_MASK))
    { startBakeFromInt(); }
#endif // defined(ENABLE_SIMPLIFIED_MODE_BAKE)

#if defined(ENABLE_VOICE_SENSOR)
//  // Voice detection is a falling edge.
//  // Handler routine not required/expected to 'clear' this interrupt.
//  // FIXME: ensure that Voice.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
//  if((changes & VOICE_INT_MASK) && !(pins & VOICE_INT_MASK))
  // Voice detection is a RISING edge.
  // Handler routine not required/expected to 'clear' this interrupt.
  // FIXME: ensure that Voice.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
  if((changes & VOICE_INT_MASK) && (pins & VOICE_INT_MASK))
    { Voice.handleInterruptSimple(); }
#endif // defined(ENABLE_VOICE_SENSOR)

  // TODO: MODE button and other things...

  // If an interrupt arrived from no other masked source then wake the CLI.
  // The will ensure that the CLI is active, eg from RX activity,
  // eg it is possible to wake the CLI subsystem with an extra CR or LF.
  // It is OK to trigger this from other things such as button presses.
  // FIXME: ensure that resetCLIActiveTimer() is inlineable to minimise ISR prologue/epilogue time and space.
  if(!(changes & MASK_PD & ~1)) { resetCLIActiveTimer(); }
  }
#endif

#endif // !defined(ALT_MAIN_LOOP) // Do not define handlers here when alt main is in use.


#if defined(ENABLE_BOILER_HUB)
// Ticks until locally-controlled boiler should be turned off; boiler should be on while this is positive.
// Ticks are of the main loop, ie 2s (almost always).
// Used in hub mode only.
static uint16_t boilerCountdownTicks;
// True if boiler should be on.
static bool isBoilerOn() { return(0 != boilerCountdownTicks); }
// Minutes that the boiler has been off for, allowing minimum off time to be enforced.
// Does not roll once at its maximum value (255).
// DHD20160124: starting at zero forces at least for off time after power-up before firing up boiler (good after power-cut).
static uint8_t boilerNoCallM;
// Reducing listening if quiet for a while helps reduce self-heating temperature error
// (~2C as of 2013/12/24 at 100% RX, ~100mW heat dissipation in V0.2 REV1 box) and saves some energy.
// Time thresholds could be affected by eco/comfort switch.
//#define RX_REDUCE_MIN_M 20 // Minimum minutes quiet before considering reducing RX duty cycle listening for call for heat; [1--255], 10--60 typical.
// IF DEFINED then give backoff threshold to minimise duty cycle.
//#define RX_REDUCE_MAX_M 240 // Minutes quiet before considering maximally reducing RX duty cycle; ]RX_REDUCE_MIN_M--255], 30--240 typical.

// Set true on receipt of plausible call for heat,
// to be polled, evaluated and cleared by the main control routine.
// Marked volatile to allow thread-safe lock-free access.
static volatile bool receivedCallForHeat;
// ID of remote caller-for-heat; only valid if receivedCallForHeat is true.
// Marked volatile to allow access from an ISR,
// but note that access may only be safe with interrupts disabled as not a byte value.
static volatile uint16_t receivedCallForHeatID;

// Raw notification of received call for heat from remote (eg FHT8V) unit.
// This form has a 16-bit ID (eg FHT8V housecode) and percent-open value [0,100].
// Note that this may include 0 percent values for a remote unit explicitly confirming
// that is is not, or has stopped, calling for heat (eg instead of replying on a timeout).
// This is not filtered, and can be delivered at any time from RX data, from a non-ISR thread.
// Does not have to be thread-/ISR- safe.
void remoteCallForHeatRX(const uint16_t id, const uint8_t percentOpen)
  {
  // TODO: Should be filtering first by housecode
  // then by individual and tracked aggregate valve-open percentage.
  // Only individual valve levels used here; no state is retained.

  // Normal minimum single-valve percentage open that is not ignored.
  // Somewhat higher than typical per-valve minimum,
  // to help provide boiler with an opportunity to dump heat before switching off.
  // May be too high to respond to valves with restricted max-open / range.
  const uint8_t default_minimum = OTRadValve::DEFAULT_VALVE_PC_SAFER_OPEN;
#ifdef ENABLE_NOMINAL_RAD_VALVE
  const uint8_t minvro = OTV0P2BASE::fnmax(default_minimum, NominalRadValve.getMinValvePcReallyOpen());
#else
  const uint8_t minvro = default_minimum;
#endif

  // TODO-553: after over an hour of continuous boiler running
  // raise the percentage threshold to successfully call for heat (for a while).
  // The aim is to allow a (combi) boiler to have reached maximum efficiency
  // and to have potentially made a significant difference to room temperature
  // but then turn off for a short while if demand is a little lower
  // to allow it to run a little harder/better when turned on again.
  // Most combis have power far higher than needed to run rads at full blast
  // and have only limited ability to modulate down,
  // so may end up cycling anyway while running the circulation pump if left on.
  // Modelled on DHD habit of having many 15-minute boiler timer segments
  // in 'off' period even during the day for many many years!
  //
  // Note: could also consider pause if mains frequency is low indicating grid stress.
  const uint8_t boilerCycleWindowMask = 0x3f;
  const uint8_t boilerCycleWindow = (minuteCount & boilerCycleWindowMask);
  const bool considerPause = (boilerCycleWindow < (boilerCycleWindowMask >> 2));

  // Equally the threshold could be lowered in the period after a possible pause (TODO-593, TODO-553)
  // to encourage the boiler to start and run harder
  // and to get a little closer to target temperatures.
  const bool encourageOn = !considerPause && (boilerCycleWindow < (boilerCycleWindowMask >> 1));

  // TODO-555: apply some basic hysteresis to help reduce boiler short-cycling.
  // Try to force a higher single-valve-%age threshold to start boiler if off,
  // at a level where at least a single valve is moderately open.
  // Selecting "quick heat" at a valve should immediately pass this,
  // as should normal warm in cold but newly-occupied room (TODO-593).
  // (This will not provide hysteresis for very high minimum really-open valve values.)
  // Be slightly tolerant with the 'moderately open' threshold
  // to allow quick start from a range of devices (TODO-593)
  // and in the face of imperfect rounding/conversion to/from percentages over the air.
  const uint8_t threshold = (!considerPause && (encourageOn || isBoilerOn())) ?
      minvro : OTV0P2BASE::fnmax(minvro, (uint8_t) (OTRadValve::DEFAULT_VALVE_PC_MODERATELY_OPEN-1));

  if(percentOpen >= threshold)
    // && FHT8VHubAcceptedHouseCode(command.hc1, command.hc2))) // Accept if house code OK.
    {
    receivedCallForHeat = true; // FIXME
    receivedCallForHeatID = id;
    }
  }
#endif


#if defined(ENABLE_RADIO_RX)
// Returns true if continuous background RX has been set up.
static bool setUpContinuousRX()
  {
  // Possible paranoia...
  // Periodically (every few hours) force radio off or at least to be not listening.
  if((30 == TIME_LSD) && (128 == minuteCount)) { PrimaryRadio.listen(false); }

#if defined(ENABLE_CONTINUOUS_RX)
  // IF IN CENTRAL HUB MODE: listen out for OpenTRV units calling for heat.
  // Power optimisation 1: when >> 1 TX cycle (of ~2mins) need not listen, ie can avoid enabling receiver.
  // Power optimisation 2: TODO: when (say) >>30m since last call for heat then only sample listen for (say) 3 minute in 10 (not at a TX cycle multiple).
  // TODO: These optimisation are more important when hub unit is running a local valve
  // to avoid temperature over-estimates from self-heating,
  // and could be disabled if no local valve is being run to provide better response to remote nodes.
#ifdef ENABLE_DEFAULT_ALWAYS_RX
  const bool needsToListen = true; // By default listen if always doing RX.
#else
  bool needsToListen = inHubMode(); // By default assume no need to listen unless in hub mode.
#endif

#if 0 && defined(DEBUG) && defined(ENABLE_DEFAULT_ALWAYS_RX)
  const int8_t listenChannel = PrimaryRadio.getListenChannel();
 if(listenChannel < 0)
    {
    DEBUG_SERIAL_PRINT_FLASHSTRING("LISTEN CHANNEL ");
    DEBUG_SERIAL_PRINT(listenChannel);
    DEBUG_SERIAL_PRINTLN();
    }
#if 0 && defined(ENABLE_RADIO_RFM23B) // ONLY IF PrimaryRadio really is RFM23B!
    const uint8_t rmode = RFM23B.getMode();
    DEBUG_SERIAL_PRINT_FLASHSTRING("RFM23B mode ");
    DEBUG_SERIAL_PRINT(rmode);
    DEBUG_SERIAL_PRINTLN();
#endif
#endif

  // Act on eavesdropping need, setting up or clearing down hooks as required.
  PrimaryRadio.listen(needsToListen);

  if(needsToListen)
    {
#if 1 && defined(DEBUG) && defined(ENABLE_RADIO_RX) && !defined(ENABLE_TRIMMED_MEMORY)
    for(uint8_t lastErr; 0 != (lastErr = PrimaryRadio.getRXErr()); )
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("!RX err ");
      DEBUG_SERIAL_PRINT(lastErr);
      DEBUG_SERIAL_PRINTLN();
      }
    const uint8_t dropped = PrimaryRadio.getRXMsgsDroppedRecent();
    static uint8_t oldDropped;
    if(dropped != oldDropped)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("!RX DROP ");
      DEBUG_SERIAL_PRINT(dropped);
      DEBUG_SERIAL_PRINTLN();
      oldDropped = dropped;
      }
#endif
#if 0 && defined(DEBUG) && !defined(ENABLE_TRIMMED_MEMORY)
    // Filtered out messages are not an error.
    const uint8_t filtered = PrimaryRadio.getRXMsgsFilteredRecent();
    static uint8_t oldFiltered;
    if(filtered != oldFiltered)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("RX filtered ");
      DEBUG_SERIAL_PRINT(filtered);
      DEBUG_SERIAL_PRINTLN();
      oldFiltered = filtered;
      }
#endif
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINT_FLASHSTRING("hub listen, on/cd ");
    DEBUG_SERIAL_PRINT(boilerCountdownTicks);
    DEBUG_SERIAL_PRINT_FLASHSTRING("t quiet ");
    DEBUG_SERIAL_PRINT(boilerNoCallM);
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("m");
#endif
    }
  return(needsToListen);
#else
  return(false);
#endif // defined(ENABLE_CONTINUOUS_RX)
  }
#endif // defined(ENABLE_RADIO_RX)

// Process calls for heat, ie turn boiler on and off as appropriate.
// Has control of OUT_HEATCALL if defined(ENABLE_BOILER_HUB).
static void processCallsForHeat(const bool second0)
  {
#if defined(ENABLE_BOILER_HUB)
  if(inHubMode())
    {
    // Check if call-for-heat has been received, and clear the flag.
    bool _h;
    uint16_t _hID; // Only valid if _h is true.
    ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
      {
      _h = receivedCallForHeat;
      if(_h)
        {
        _hID = receivedCallForHeatID;
        receivedCallForHeat = false;
        }
      }
    const bool heardIt = _h;
    const uint16_t hcRequest = heardIt ? _hID : 0; // Only valid if heardIt is true.
    
//    // Don't log call for hear if near overrun,
//    // and leave any error queued for next time.
//    if(OTV0P2BASE::getSubCycleTime() >= nearOverrunThreshold) { } // { tooNearOverrun = true; }
//    else
      {
      if(heardIt)
        {
//        DEBUG_SERIAL_TIMESTAMP();
//        DEBUG_SERIAL_PRINT(' ');
        OTV0P2BASE::serialPrintAndFlush(F("CfH ")); // Call for heat from
        OTV0P2BASE::serialPrintAndFlush((hcRequest >> 8) & 0xff);
        OTV0P2BASE::serialPrintAndFlush(' ');
        OTV0P2BASE::serialPrintAndFlush(hcRequest & 0xff);
        OTV0P2BASE::serialPrintlnAndFlush();
        }
      }

    // Record call for heat, both to start boiler-on cycle and possibly to defer need to listen again.
    // Ignore new calls for heat until minimum off/quiet period has been reached.
    // Possible optimisation: may be able to stop RX if boiler is on for local demand (can measure local temp better: less self-heating) and not collecting stats.
    if(heardIt)
      {
      const uint8_t minOnMins = getMinBoilerOnMinutes();
      bool ignoreRCfH = false;
      if(!isBoilerOn())
        {
        // Boiler was off.
        // Ignore new call for heat if boiler has not been off long enough,
        // forcing a time longer than the specified minimum,
        // regardless of when second0 happens to be.
        // (The min(254, ...) is to ensure that the boiler can come on even if minOnMins == 255.)
        // TODO: randomly extend the off-time a little (eg during grid stress) partly to randmonise whole cycle length.
        if(boilerNoCallM <= min(254, minOnMins)) { ignoreRCfH = true; }
//        if(OTV0P2BASE::getSubCycleTime() >= nearOverrunThreshold) { } // { tooNearOverrun = true; }
//        else
          if(ignoreRCfH) { OTV0P2BASE::serialPrintlnAndFlush(F("RCfH-")); } // Remote call for heat ignored.
        else { OTV0P2BASE::serialPrintlnAndFlush(F("RCfH1")); } // Remote call for heat on.
        }
      if(!ignoreRCfH)
        {
        const uint16_t onTimeTicks = minOnMins * (uint16_t) (60U / OTV0P2BASE::MAIN_TICK_S);
        // Restart count-down time (keeping boiler on) with new call for heat.
        boilerCountdownTicks = onTimeTicks;
        boilerNoCallM = 0; // No time has passed since the last call.
        }
      }

    // If boiler is on, then count down towards boiler off.
    if(isBoilerOn())
      {
      if(0 == --boilerCountdownTicks)
        {
        // Boiler should now be switched off.
//        if(OTV0P2BASE::getSubCycleTime() >= nearOverrunThreshold) { } // { tooNearOverrun = true; }
//        else 
          { OTV0P2BASE::serialPrintlnAndFlush(F("RCfH0")); } // Remote call for heat off
        }
      }
    // Else boiler is off so count up quiet minutes until at max...
    else if(second0 && (boilerNoCallM < 255))
        { ++boilerNoCallM; }

    // Set BOILER_OUT as appropriate for calls for heat.
    // Local calls for heat come via the same route (TODO-607).
    fastDigitalWrite(OUT_HEATCALL, (isBoilerOn() ? HIGH : LOW));
    }
  // Force boiler off when not in hub mode.
  else { fastDigitalWrite(OUT_HEATCALL, LOW); }
#endif // defined(ENABLE_BOILER_HUB)
  }


// Main loop for OpenTRV radiator control.
// Note: exiting and re-entering can take a little while, handling Arduino background tasks such as serial.
void loopOpenTRV()
  {
#if 0 && defined(DEBUG) // Indicate loop start.
  DEBUG_SERIAL_PRINT('L');
  DEBUG_SERIAL_PRINT(TIME_LSD);
  DEBUG_SERIAL_PRINTLN();
#endif

  // Set up some variables before sleeping to minimise delay/jitter after the RTC tick.
  bool showStatus = false; // Show status at end of loop?

  // Use the zeroth second in each minute to force extra deep device sleeps/resets, etc.
  const bool second0 = (0 == TIME_LSD);
  // Sensor readings are taken late in each minute (where they are taken)
  // and if possible noise and heat and light should be minimised in this part of each minute to improve readings.
//  const bool sensorReading30s = (TIME_LSD >= 30);
  // Sensor readings and (stats transmissions) are nominally on a 4-minute cycle.
  const uint8_t minuteFrom4 = (minuteCount & 3);
  // The 0th minute in each group of four is always used for measuring where possible (possibly amongst others)
  // and where possible locally-generated noise and heat and light should be minimised in this minute
  // to give the best possible readings.
  // True if this is the first (0th) minute in each group of four.
  const bool minute0From4ForSensors = (0 == minuteFrom4);
  // True if this is the minute after all sensors should have been sampled.
  const bool minute1From4AfterSensors = (1 == minuteFrom4);

  // Note last-measured battery status.
  const bool batteryLow = Supply_cV.isSupplyVoltageLow();

  // Run some tasks less often when not demanding heat (at the valve or boiler), so as to conserve battery/energy.
  // Spare the batteries if they are low, or the unit is in FROST mode, or if the room/area appears to be vacant.
  // Stay responsive if the valve is open and/or we are otherwise calling for heat.
  const bool conserveBattery =
    (batteryLow || !inWarmMode() || Occupancy.longVacant()) &&
#if defined(ENABLE_BOILER_HUB)
    (!isBoilerOn()) && // Unless the boiler is off, stay responsive.
#endif
#if defined(ENABLE_NOMINAL_RAD_VALVE) && defined(LOCAL_VALVE)
//    (!NominalRadValve.isControlledValveReallyOpen()); // &&  // Run at full speed until valve(s) should actually have shut and the boiler gone off.
    (!NominalRadValve.isCallingForHeat()); // Run at full speed until not nominally demanding heat, eg even during FROST mode or pre-heating.
#else
    true; // Allow local power conservation if all other factors are right.
#endif

  // Try if very near to end of cycle and thus causing an overrun.
  // Conversely, if not true, should have time to safely log outputs, etc.
  const uint8_t nearOverrunThreshold = OTV0P2BASE::GSCT_MAX - 8; // ~64ms/~32 serial TX chars of grace time...
//  bool tooNearOverrun = false; // Set flag that can be checked later.

//  if(getSubCycleTime() >= nearOverrunThreshold) { tooNearOverrun = true; }

#if defined(ENABLE_CONTINUOUS_RX)
  const bool needsToListen = setUpContinuousRX();
#endif

#if defined(ENABLE_BOILER_HUB)
  // Set BOILER_OUT as appropriate for calls for heat.
  processCallsForHeat(second0);
#endif


  // Sleep in low-power mode (waiting for interrupts) until seconds roll.
  // NOTE: sleep at the top of the loop to minimise timing jitter/delay from Arduino background activity after loop() returns.
  // DHD20130425: waking up from sleep and getting to start processing below this block may take >10ms.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("*E"); // End-of-cycle sleep.
#endif
//  // Ensure that serial I/O is off while sleeping, unless listening with radio.
//  if(!needsToListen) { powerDownSerial(); } else { powerUpSerialIfDisabled<V0P2_UART_BAUD>(); }
  // Ensure that serial I/O is off while sleeping.
  OTV0P2BASE::powerDownSerial();
  // Power down most stuff (except radio for hub RX).
  OTV0P2BASE::minimisePowerWithoutSleep();
  uint_fast8_t newTLSD;
  while(TIME_LSD == (newTLSD = OTV0P2BASE::getSecondsLT()))
    {
#ifdef ENABLE_RADIO_RX
    // Poll I/O and process message incrementally (in this otherwise idle time)
    // before sleep and on wakeup in case some IO needs further processing now,
    // eg work was accrued during the previous major slow/outer loop
    // or the in a previous orbit of this loop sleep or nap was terminated by an I/O interrupt.
    // May generate output to host on Serial.
    // Come back and have another go immediately until no work remaining.
    if(handleQueuedMessages(&Serial, true, &PrimaryRadio)) { continue; }
#endif

// If missing h/w interrupts for anything that needs rapid response
// then AVOID the lowest-power long sleep.
#if defined(ENABLE_CONTINUOUS_RX) && !defined(PIN_RFM_NIRQ)
    if(needsToListen)
#else
    if(false)
#endif
      {
      // If there is not hardware interrupt wakeup on receipt of a frame,
      // then this can only sleep for a short time between explicit poll()s,
      // though in any case allow wake on interrupt to minimise loop timing jitter
      // when the slow RTC 'end of sleep' tick arrives.
      OTV0P2BASE::nap(WDTO_15MS, true);
      }
    else
      {
      // Normal long minimal-power sleep until wake-up interrupt.
      // Rely on interrupt to force quick loop round to I/O poll.
      OTV0P2BASE::sleepUntilInt();
      }
//    DEBUG_SERIAL_PRINTLN_FLASHSTRING("w"); // Wakeup.
    }
  TIME_LSD = newTLSD;
#if defined(ENABLE_WATCHDOG_SLOW)
  // Reset and immediately re-prime the RTC-based watchdog.
  OTV0P2BASE::resetRTCWatchDog();
  OTV0P2BASE::enableRTCWatchdog(true);
#endif
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("*S"); // Start-of-cycle wake.
#endif

//#if defined(ENABLE_BOILER_HUB) && defined(ENABLE_FHT8VSIMPLE) // Deal with FHT8V eavesdropping if needed.
//  // Check RSSI...
//  if(needsToListen)
//    {
//    const uint8_t rssi = RFM23.getRSSI();
//    static uint8_t lastRSSI;
//    if((rssi > 0) && (lastRSSI != rssi))
//      {
//      lastRSSI = rssi;
//      addEntropyToPool(rssi, 0); // Probably some real entropy but don't assume it.
//#if 0 && defined(DEBUG)
//      DEBUG_SERIAL_PRINT_FLASHSTRING("RSSI=");
//      DEBUG_SERIAL_PRINT(rssi);
//      DEBUG_SERIAL_PRINTLN();
//#endif
//      }
//    }
//#endif

#if 0 && defined(DEBUG) // Show CPU cycles.
  DEBUG_SERIAL_PRINT('C');
  DEBUG_SERIAL_PRINT(cycleCountCPU());
  DEBUG_SERIAL_PRINTLN();
#endif


  // START LOOP BODY
  // ===============


//  // Warn if too near overrun before.
//  if(tooNearOverrun) { OTV0P2BASE::serialPrintlnAndFlush(F("?near overrun")); }


  // Get current power supply voltage.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Vcc: ");
  DEBUG_SERIAL_PRINT(Supply_mV.read());
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("mV");
#endif


#if defined(ENABLE_FHT8VSIMPLE)
  // Try for double TX for more robust conversation with valve unless:
  //   * battery is low
  //   * the valve is not required to be wide open (ie a reasonable temperature is currently being maintained).
  //   * this is a hub and has to listen as much as possible
  // to conserve battery and bandwidth.
  #ifdef ENABLE_NOMINAL_RAD_VALVE
  const bool doubleTXForFTH8V = !conserveBattery && !inHubMode() && (NominalRadValve.get() >= 50);
  #else
  const bool doubleTXForFTH8V = false;
  #endif
  // FHT8V is highest priority and runs first.
  // ---------- HALF SECOND #0 -----------
  bool useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8V.FHT8VPollSyncAndTX_First(doubleTXForFTH8V); // Time for extra TX before UI.
//  if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@0"); }
#endif


  // High-priority UI handing, every other/even second.
  // Show status if the user changed something significant.
  // Must take ~300ms or less so as not to run over into next half second if two TXs are done.
  bool recompute = false; // Set true if an extra recompute of target temperature should be done.
#if !defined(V0P2BASE_TWO_S_TICK_RTC_SUPPORT)
  if(0 == (TIME_LSD & 1))
#endif
    {
#ifdef ENABLE_FULL_OT_UI
    // Run the OpenTRV button/LED UI if required.
    if(tickUI(TIME_LSD))
      {
      showStatus = true;
      recompute = true;
      }
#endif
    }

  // Handling the UI may have taken a little while, so process I/O a little.
  handleQueuedMessages(&Serial, true, &PrimaryRadio); // Deal with any pending I/O.


#ifdef ENABLE_MODELLED_RAD_VALVE
  if(recompute || veryRecentUIControlUse())
    {
    // Force immediate recompute of target temperature for (UI) responsiveness.
    NominalRadValve.computeTargetTemperature();
    // Keep dynamic adjustment of sensors up to date.
    updateSensorsFromStats();
    }
#endif


#if defined(ENABLE_FHT8VSIMPLE)
  if(useExtraFHT8VTXSlots)
    {
    // Time for extra TX before other actions, but don't bother if minimising power in frost mode.
    // ---------- HALF SECOND #1 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8V.FHT8VPollSyncAndTX_Next(doubleTXForFTH8V);
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@1"); }
    // Handling the FHT8V may have taken a little while, so process I/O a little.
    handleQueuedMessages(&Serial, true, &PrimaryRadio); // Deal with any pending I/O.
    }
#endif


  // DO SCHEDULING

  // Once-per-minute tasks: all must take << 0.3s unless particular care is taken.
  // Run tasks spread throughout the minute to be as kind to batteries (etc) as possible.
  // Only when runAll is true run less-critical tasks that be skipped sometimes when particularly conserving energy.
  // Run all for first full 4-minute cycle, eg because unit may start anywhere in it.
  // Note: ensure only take ambient light reading at times when all LEDs are off (or turn them off).
  // TODO: coordinate temperature reading with time when radio and other heat-generating items are off for more accurate readings.
  const bool runAll = (!conserveBattery) || minute0From4ForSensors || (minuteCount < 4);

  switch(TIME_LSD) // With V0P2BASE_TWO_S_TICK_RTC_SUPPORT only even seconds are available.
    {
    case 0:
      {
      // Tasks that must be run every minute.
      ++minuteCount; // Note simple roll-over to 0 at max value.
      checkUserSchedule(); // Force to user's programmed settings, if any, at the correct time.
      // Ensure that the RTC has been persisted promptly when necessary.
      OTV0P2BASE::persistRTC();
      // Run hourly tasks at the end of the hour.
      if(59 == OTV0P2BASE::getMinutesLT()) { endOfHourTasks(); }
      break;
      }

    // Churn/reseed PRNG(s) a little to improve unpredictability in use: should be lightweight.
    case 2: { if(runAll) { OTV0P2BASE::seedRNG8(minuteCount ^ OTV0P2BASE::getCPUCycleCount() ^ (uint8_t)Supply_cV.get(), OTV0P2BASE::_getSubCycleTime() ^ AmbLight.get(), (uint8_t)TemperatureC16.get()); } break; }
    // Force read of supply/battery voltage; measure and recompute status (etc) less often when already thought to be low, eg when conserving.
    case 4: { if(runAll) { Supply_cV.read(); } break; }

#if defined(ENABLE_STATS_TX)
    // Periodic transmission of stats if NOT driving a local valve (else stats can be piggybacked onto that).
    // Randomised somewhat between slots and also within the slot to help avoid collisions.
    static uint8_t txTick;
    case 6: { txTick = OTV0P2BASE::randRNG8() & 7; break; } // Pick which of the 8 slots to use.
    case 8: case 10: case 12: case 14: case 16: case 18: case 20: case 22:
      {
      // Only the slot where txTick is zero is used.
      if(0 != txTick--) { break; }

#if defined(ENABLE_FHT8VSIMPLE)
      // Avoid transmit conflict with FS20; just drop the slot.
      // We should possibly choose between this and piggybacking stats to avoid busting duty-cycle rules.
      if(useExtraFHT8VTXSlots && localFHT8VTRVEnabled()) { break; }
#endif

#if !defined(ENABLE_FREQUENT_STATS_TX) // If ENABLE_FREQUENT_STATS_TX then send every minute regardless.
      // Stats TX in the minute after all sensors should have been polled (so that readings are fresh).
      // Usually send one frame every 4 minutes, else abort,
      // but occasionally send otherwise to make (secure) traffic analysis harder,
      // though not enough to make a significant difference to bandwidth.
      // Send very slightly more often when changed stats pending to send upstream.
      // TODO: send immediately with 100% valve payload when user puts system into BAKE mode for fast response.
      if(!minute1From4AfterSensors && (OTV0P2BASE::randRNG8() > (ss1.changedValue() ? 4 : 3))) { break; }
#endif

      // Abort if not allowed to send stats at all.
      // FIXME: fix this to send bare calls for heat / valve % instead from valves for secure non-FHT8V comms.
      if(!enableTrailingStatsPayload()) { break; }

      // Sleep randomly up to ~25% of the minor cycle
      // to spread transmissions and thus help avoid collisions.
      // (Longer than 25%/0.5s could interfere with other ops such as FHT8V TXes.)
      const uint8_t stopBy = 1 + (((OTV0P2BASE::GSCT_MAX >> 2) | 7) & OTV0P2BASE::randRNG8());
      while(OTV0P2BASE::getSubCycleTime() <= stopBy)
        {
        // Handle any pending I/O while waiting.
        if(handleQueuedMessages(&Serial, true, &PrimaryRadio)) { continue; }
        // Sleep a little.
        OTV0P2BASE::nap(WDTO_15MS, true);
        }

      // Send stats!
      // Try for double TX for extra robustness unless:
      //   * this is a speculative 'extra' TX
      //   * battery is low
      //   * this node is a hub so needs to listen as much as possible
      // This doesn't generally/always need to send binary/both formats
      // if this is controlling a local FHT8V on which the binary stats can be piggybacked.
      // Ie, if doesn't have a local TRV then it must send binary some of the time.
      // Any recently-changed stats value is a hint that a strong transmission might be a good idea.
#if defined(ENABLE_BINARY_STATS_TX) && defined(ENABLE_FS20_ENCODING_SUPPORT)
      const bool doBinary = !localFHT8VTRVEnabled() && OTV0P2BASE::randRNG8NextBoolean();
#else
      const bool doBinary = false;
#endif
      bareStatsTX(!batteryLow && !inHubMode() && ss1.changedValue(), doBinary);
      break;
      }
#endif // defined(ENABLE_STATS_TX)

#if defined(ENABLE_SECURE_RADIO_BEACON)
    // Send a small secure radio beacon "I'm alive!" message regularly if configured.
    case 30:
      {
#if 1 && defined(DEBUG)
      DEBUG_SERIAL_PRINT_FLASHSTRING("Beacon TX... ");
#endif
      // Get the 'building' key for broadcast.
      uint8_t key[16];
      if(!OTV0P2BASE::getPrimaryBuilding16ByteSecretKey(key))
        {
#if 1 && defined(DEBUG)
        DEBUG_SERIAL_PRINTLN_FLASHSTRING("!failed (no key)");
#endif
        break;
        }
      const OTRadioLink::SimpleSecureFrame32or0BodyTXBase::fixed32BTextSize12BNonce16BTagSimpleEnc_ptr_t e = OTAESGCM::fixed32BTextSize12BNonce16BTagSimpleEnc_DEFAULT_STATELESS;
      const uint8_t txIDLen = OTRadioLink::ENC_BODY_DEFAULT_ID_BYTES;
      uint8_t buf[OTRadioLink::generateSecureBeaconMaxBufSize];
      const uint8_t bodylen = OTRadioLink::generateSecureBeaconRawForTX(buf, sizeof(buf), txIDLen, e, NULL, key);
      // ASSUME FRAMED CHANNEL 0 (but could check with config isUnframed flag).
      // When sending on a channel with framing, do not explicitly send the frame length byte.
      // DO NOT attempt to send if construction of the secure frame failed;
      // doing so may reuse IVs and destroy the cipher security.
      const bool success = (0 != bodylen) && PrimaryRadio.sendRaw(buf+1, bodylen-1);
#if 1 && defined(DEBUG)
      DEBUG_SERIAL_PRINT(success);
      DEBUG_SERIAL_PRINTLN();
#endif
      break;
      }
#endif // defined(ENABLE_SECURE_RADIO_BEACON)

// SENSOR READ AND STATS
//
// All external sensor reads should be in the second half of the minute (>32) if possible.
// This is to have them as close to stats collection at the end of the minute as possible,
// and to allow randomisation of the start-up cycle position in the first 32s to help avoid inter-unit collisions.
// Also all sources of noise, self-heating, etc, may be turned off for the 'sensor read minute'
// and thus will have diminished by this point.

#ifdef ENABLE_VOICE_SENSOR
    // Poll voice detection sensor at a fixed rate.
    case 46: { Voice.read(); break; }
#endif

#ifdef TEMP_POT_AVAILABLE
    // Sample the user-selected WARM temperature target at a fixed rate.
    // This allows the unit to stay reasonably responsive to adjusting the temperature dial.
    case 48: { TempPot.read(); break; }
#endif

    // Read all environmental inputs, late in the cycle.
#ifdef HUMIDITY_SENSOR_SUPPORT
    // Sample humidity.
    case 50: { if(runAll) { RelHumidity.read(); } break; }
#endif

#if defined(ENABLE_AMBLIGHT_SENSOR)
    // Poll ambient light level at a fixed rate.
    // This allows the unit to respond consistently to (eg) switching lights on (eg TODO-388).
    case 52:
      {
      // Force all UI lights off before sampling ambient light level.
      LED_HEATCALL_OFF();
#if defined(LED_UI2_EXISTS) && defined(ENABLE_UI_LED_2_IF_AVAILABLE)
      // Turn off second UI LED if available.
      LED_UI2_OFF();
#endif
      AmbLight.read();
      break;
      }
#endif

    // At a hub, sample temperature regularly as late as possible in the minute just before recomputing valve position.
    // Force a regular read to make stats such as rate-of-change simple and to minimise lag.
    // TODO: optimise to reduce power consumption when not calling for heat.
    // TODO: optimise to reduce self-heating jitter when in hub/listen/RX mode.
    case 54: { TemperatureC16.read(); break; }

    // Compute targets and heat demand based on environmental inputs and occupancy.
    // This should happen as soon after the latest readings as possible (temperature especially).
    case 56:
      {
#if defined(ENABLE_OCCUPANCY_SUPPORT)
      // Update occupancy measures that partially use rolling stats.
#if defined(ENABLE_OCCUPANCY_DETECTION_FROM_RH) && defined(HUMIDITY_SENSOR_SUPPORT)
      // If RH% is rising fast enough then take this a mild occupancy indicator.
      // Suppress this if temperature is falling since RH% change may be misleading.  (TODO-696)
      // Suppress this in the dark to avoid nuisance behaviour
      // (if there is a working ambient light sensor, else don't suppress),
      // even if not a false positive (ie the room is occupied, by a sleeper),
      // such as a valve opening and/or the boiler firing up at night.
      // Use a guard formulated to allow the RH%-based detection to work
      // if ambient light sensing is disabled,
      // eg allow RH%-based sensing unless known to be dark.
      if(runAll && // Only if all sensors have been refreshed.
         !AmbLight.isRoomDark()) // Only if room not known to be dark, from a working sensor.
        {
        // Only continue if temperature appears not to be falling compared to previous hour (TODO-696).
        // No previous temperature will show as a very large number so should fail safe.
        // Note use of compress/expand to try to get round companding granularity issues.
        if(OTV0P2BASE::expandTempC16(OTV0P2BASE::compressTempC16(TemperatureC16.get())) >= OTV0P2BASE::expandTempC16(OTV0P2BASE::getByHourStat(V0P2BASE_EE_STATS_SET_TEMP_BY_HOUR, OTV0P2BASE::getPrevHourLT())))
          {
          const uint8_t lastRH = OTV0P2BASE::getByHourStat(V0P2BASE_EE_STATS_SET_RHPC_BY_HOUR, OTV0P2BASE::getPrevHourLT());
          if((OTV0P2BASE::STATS_UNSET_BYTE != lastRH) &&
             (RelHumidity.get() >= lastRH + OTV0P2BASE::HumiditySensorSHT21::HUMIDITY_OCCUPANCY_PC_MIN_RISE_PER_H))
            { Occupancy.markAsPossiblyOccupied(); }
          }
        }
#endif // defined(ENABLE_OCCUPANCY_DETECTION_FROM_RH) && defined(HUMIDITY_SENSOR_SUPPORT)

      // Update occupancy status (fresh for target recomputation) at a fixed rate.
      Occupancy.read();
#endif // defined(ENABLE_OCCUPANCY_SUPPORT)

#ifdef ENABLE_NOMINAL_RAD_VALVE
      // Recompute target, valve position and call for heat, etc.
      // Should be called once per minute to work correctly.
      NominalRadValve.read();
#endif

#if defined(ENABLE_FHT8VSIMPLE) && defined(ENABLE_LOCAL_TRV) // Only regen when needed.
      // If there was a change in target valve position,
      // or periodically in the minute after all sensors should have been read,
      // precompute some or all of any outgoing frame/stats/etc ready for the next transmission.
      if(NominalRadValve.isValveMoved() ||
         (minute1From4AfterSensors && enableTrailingStatsPayload()))
        {
        if(localFHT8VTRVEnabled()) { FHT8V.set(NominalRadValve.get() /*, NominalRadValve.isCallingForHeat() */); }
        }

#if defined(ENABLE_BOILER_HUB)
      // Feed in the local valve position when calling for heat just as if over the air.
      // (Does not arrive with the normal FHT8V timing of 2-minute gaps so boiler may turn off out of sync.)
      if(FHT8V.isControlledValveReallyOpen()) { remoteCallForHeatRX(FHT8V.nvGetHC(), FHT8V.get()); }
#endif // defined(ENABLE_BOILER_HUB)
#elif defined(ENABLE_NOMINAL_RAD_VALVE) && defined(ENABLE_LOCAL_TRV) // Other local valve types, simulate a remote call for heat with a fake ID.
#if defined(ENABLE_BOILER_HUB)
      // Feed in the local valve position when calling for heat just as if over the air.
      if(NominalRadValve.isControlledValveReallyOpen()) { remoteCallForHeatRX(~0, NominalRadValve.get()); }
#endif // defined(ENABLE_BOILER_HUB)
#endif

#if 1 && defined(DEBUG) && defined(ENABLE_BOILER_HUB) && !defined(ENABLE_TRIMMED_MEMORY)
      // Track how long since remote call for heat last heard.
      if(isBoilerOn())
        {
        DEBUG_SERIAL_PRINT_FLASHSTRING("Boiler on, s: ");
        DEBUG_SERIAL_PRINT(boilerCountdownTicks * OTV0P2BASE::MAIN_TICK_S);
        DEBUG_SERIAL_PRINTLN();
        }
#endif

      // Show current status if appropriate.
      if(runAll) { showStatus = true; }
      break;
      }

    // Stats samples; should never be missed.
    case 58:
      {
      // Take full stats sample as near the end of the hour as reasonably possible (without danger of overrun),
      // and with other optional non-full samples evenly spaced throughout the hour (if not low on battery).
      // A small even number of samples (or 1 sample) is probably most efficient; the system supports 2 max as of 20150329.
      if(minute0From4ForSensors) // Use lowest-noise samples just taken in the special 0 minute out of each 4.
        {
        const uint_least8_t mm = OTV0P2BASE::getMinutesLT();
        switch(mm)
          {
          case 26: case 27: case 28: case 29:
            { if(!batteryLow) { sampleStats(false); } break; } // Skip sub-samples if short of energy.
          case 56: case 57: case 58: case 59:
            {
            // Always take the full sample at the end of each hour.
            sampleStats(true);
            // Feed back rolling stats to sensors to set noise floors, adapt to sensors and local env...
            updateSensorsFromStats();
            break;
            }
          }
        }
      break;
      }
    }

#if defined(ENABLE_FHT8VSIMPLE) && defined(V0P2BASE_TWO_S_TICK_RTC_SUPPORT)
  if(useExtraFHT8VTXSlots)
    {
    // ---------- HALF SECOND #2 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8V.FHT8VPollSyncAndTX_Next(doubleTXForFTH8V);
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@2"); }
    // Handling the FHT8V may have taken a little while, so process I/O a little.
    handleQueuedMessages(&Serial, true, &PrimaryRadio); // Deal with any pending I/O.
    }
#endif

  // Generate periodic status reports.
  if(showStatus) { serialStatusReport(); }

#if defined(ENABLE_FHT8VSIMPLE) && defined(V0P2BASE_TWO_S_TICK_RTC_SUPPORT)
  if(useExtraFHT8VTXSlots)
    {
    // ---------- HALF SECOND #3 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8V.FHT8VPollSyncAndTX_Next(doubleTXForFTH8V);
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@3"); }
    // Handling the FHT8V may have taken a little while, so process I/O a little.
    handleQueuedMessages(&Serial, true, &PrimaryRadio); // Deal with any pending I/O.
    }
#endif

  // End-of-loop processing, that may be slow.
  // Ensure progress on queued messages ahead of slow work.  (TODO-867)
  handleQueuedMessages(&Serial, true, &PrimaryRadio); // Deal with any pending I/O.

#if defined(HAS_DORM1_VALVE_DRIVE) && defined(ENABLE_LOCAL_TRV)
  // Handle local direct-drive valve, eg DORM1.
#if defined(ENABLE_NOMINAL_RAD_VALVE)
  // Get current modelled valve position into abstract driver.
  ValveDirect.set(NominalRadValve.get());
#endif
  // If waiting for for verification that the valve has been fitted
  // then accept any manual interaction with controls as that signal.
  // ('Any' manual interaction may prove too sensitive.)
  // Also have a timeout of somewhat over ~10m from startup
  // for automatic recovery after any crash and restart.
  if(ValveDirect.isWaitingForValveToBeFitted())
      {
      if(veryRecentUIControlUse() || (minuteCount > 15))
          { ValveDirect.signalValveFitted(); }
      }
  // Provide regular poll to motor driver.
  // May take significant time to run
  // so don't call when timing is critical
  // nor when not much time left this cycle,
  // nor some of the time during startup if possible,
  // so as (for example) to allow the CLI to be operable.
  // Only calling this after most other heavy-lifting work is likely done.
  // Note that FHT8V sync will take up at least the first 1s of a 2s subcycle.
  if(!showStatus &&
     // (ValveDirect.isInNormalRunState() || (0 == (3 & TIME_LSD))) &&
     (OTV0P2BASE::getSubCycleTime() < ((OTV0P2BASE::GSCT_MAX/4)*3)))
    { ValveDirect.read(); }
#endif

  // Command-Line Interface (CLI) polling.
  // If a reasonable chunk of the minor cycle remains after all other work is done
  // AND the CLI is / should be active OR a status line has just been output
  // then poll/prompt the user for input
  // using a timeout which should safely avoid overrun, ie missing the next basic tick,
  // and which should also allow some energy-saving sleep.
#if 1 && defined(ENABLE_CLI)
  if(isCLIActive())
    {
    const uint8_t sct = OTV0P2BASE::getSubCycleTime();
    const uint8_t listenTime = OTV0P2BASE::CLI::MIN_CLI_POLL_SCT;
    const uint8_t stopBy = nearOverrunThreshold - 1;
    pollCLI(stopBy, 0 == TIME_LSD);
    }
#endif


#if 0 && defined(DEBUG)
  const int tDone = getSubCycleTime();
  if(tDone > 1) // Ignore for trivial 1-click time.
    {
    DEBUG_SERIAL_PRINT_FLASHSTRING("done in "); // Indicates what fraction of available loop time was used / 256.
    DEBUG_SERIAL_PRINT(tDone);
    DEBUG_SERIAL_PRINT_FLASHSTRING(" @ ");
    DEBUG_SERIAL_TIMESTAMP();
    DEBUG_SERIAL_PRINTLN();
    }
#endif

// Do explicit overrun detection iff RTC watchdog not enabled (should reset instead).
#if !defined(ENABLE_WATCHDOG_SLOW) // || !defined(ENABLE_TRIMMED_MEMORY) // Could reinstate if not short memory...
  // Detect and handle (actual or near) overrun, if it happens, though it should not.
  if(TIME_LSD != OTV0P2BASE::getSecondsLT())
    {
    // Increment the overrun counter (stored inverted, so 0xff initialised => 0 overruns).
    const uint8_t orc = 1 + ~eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER);
    OTV0P2BASE::eeprom_smart_update_byte((uint8_t *)V0P2BASE_EE_START_OVERRUN_COUNTER, ~orc);
#if 1 && defined(DEBUG)
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("!loop overrun");
#endif
#if defined(ENABLE_FHT8VSIMPLE)
    FHT8V.resyncWithValve(); // Assume that sync with valve may have been lost, so re-sync.
#endif
    TIME_LSD = OTV0P2BASE::getSecondsLT(); // Prepare to sleep until start of next full minor cycle.
    }
#if 0 && defined(DEBUG) // Expect to pick up near overrun at start of next loop.
  else if(getSubCycleTime() >= nearOverrunThreshold)
    {
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("?O"); // Near overrun.  Note 2ms/char to send...
    }
#endif
#endif // !defined(ENABLE_WATCHDOG_SLOW)
  }
