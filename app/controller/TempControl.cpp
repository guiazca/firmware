/*
 * Copyright 2012-2013 BrewPi/Elco Jacobs.
 *
 * This file is part of BrewPi.
 *
 * BrewPi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BrewPi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BrewPi.  If not, see <http://www.gnu.org/licenses/>.
 */



#include "Brewpi.h"
#include "TemperatureFormats.h"
#include "Board.h"
#include "TempControl.h"
#include "PiLink.h"
#include "TempSensor.h"
#include "Ticks.h"
#include "TempSensorMock.h"
#include "EepromManager.h"
#include "TempSensorDisconnected.h"
#include "ModeControl.h"
#include "fixstl.h"

TempControl                   tempControl;

#if TEMP_CONTROL_STATIC
extern ValueSensor<bool>      defaultSensor;
extern ValueActuator          defaultActuator;
extern DisconnectedTempSensor defaultTempSensor;

// These sensors are switched out to implement multi-chamber.
TempSensor *      TempControl::beerSensor;
TempSensor *      TempControl::fridgeSensor;
BasicTempSensor * TempControl::ambientSensor = &defaultTempSensor;
Actuator *        TempControl::light         = &defaultActuator;
Actuator *        TempControl::fan           = &defaultActuator;
ValueActuator     cameraLightState;
ActuatorOnOff *   TempControl::chamberCoolerLimiter;
ActuatorPwm *     TempControl::chamberHeater;
ActuatorPwm *     TempControl::chamberCooler;
ActuatorPwm *     TempControl::beerHeater;
AutoOffActuator   TempControl::cameraLight(600, &cameraLightState);    // timeout 10 min
Sensor<bool> *    TempControl::door = &defaultSensor;

// Control parameters
ControlConstants TempControl::cc;
ControlSettings  TempControl::cs;
ControlVariables TempControl::cv;

// State variables
states TempControl::state;
bool   TempControl::doorOpen;

// keep track of beer setting stored in EEPROM
temperature TempControl::storedBeerSetting;

// Timers
tcduration_t                      TempControl::lastIdleTime;
tcduration_t                      TempControl::lastHeatTime;
tcduration_t                      TempControl::lastCoolTime;
#endif

ControlConstants const ccDefaults PROGMEM =
{
    // Do Not change the order of these initializations!

    /* tempFormat */
    'C',

    /* tempSettingMin */
    intToTemp(1),    // +1 deg Celsius

    /* tempSettingMax */
    intToTemp(110),    // +112 deg Celsius

    // control defines, also in fixed point format (7 int bits, 9 frac bits), so multiplied by 2^9=512

    /* Kp */
    doubleToTempDiff(5.0),    // +5

    /* Ki */
    doubleToTempDiff(0.25),    // +0.25

    /* Kd */
    doubleToTempDiff(1.5),    // -1.5

    /* iMaxError */
    doubleToTempDiff(1),    // 1 deg

    // Stay Idle when fridge temperature is in this range

    /* idleRangeHigh */
    doubleToTempDiff(0.1),    // +0.1 deg Celsius

    /* idleRangeLow */
    doubleToTempDiff(-0.1),    // -0.1 deg Celsius

    // Set filter coefficients. This is the b value. See FilterFixed.h for delay times.
    // The delay time is 3.33 * 2^b * number of cascades

    /* fridgeFastFilter */
    1u,

    /* fridgeSlowFilter */
    4u,

    /* fridgeSlopeFilter */
    3u,

    /* beerFastFilter */
    3u,

    /* beerSlowFilter */
    4u,

    /* beerSlopeFilter */
    4u,

    /* lightAsHeater */
    0,

    /* rotaryHalfSteps */
    0,

    /* pidMax */
    intToTempDiff(10),    // +/- 10 deg Celsius

    /* heatPwmPeriod */
    4, // 4 seconds

    /* coolPwmPeriod */
    600, // 10 minutes

    /* fridgePwmKpHeat */
    intToTempDiff(20),

    /* fridgePwmKiHeat */
    intToTempDiff(2),

    /* fridgePwmKpCool */
    intToTempDiff(20),

    /* fridgePwmKiCool */
    intToTempDiff(2),

    /* beerPwmKpHeat */
    intToTempDiff(20),

    /* beerPwmKiHeat */
    intToTempDiff(2)
};

TempControl::TempControl()
{
}

;
void TempControl::init(void)
{
    state   = IDLE;
    cs.mode = MODE_OFF;

    cameraLight.setActive(false);

    // this is for cases where the device manager hasn't configured beer/fridge sensor.
    if (beerSensor == NULL)
    {
        beerSensor = new TempSensor(TEMP_SENSOR_TYPE_BEER, &defaultTempSensor);

        beerSensor -> init();
    }

    if (fridgeSensor == NULL)
    {
        fridgeSensor = new TempSensor(TEMP_SENSOR_TYPE_FRIDGE, &defaultTempSensor);

        fridgeSensor -> init();
    }

    if (chamberHeater == NULL)
    {
        chamberHeater = new ActuatorPwm(&defaultActuator, ccDefaults.heatPwmPeriod);
    }

    if (chamberCooler == NULL)
    {
        chamberCoolerLimiter = new ActuatorOnOff(&defaultActuator); // time limited actuator
        chamberCooler = new ActuatorPwm(chamberCoolerLimiter, ccDefaults.coolPwmPeriod);
    }

    if (beerHeater == NULL)
    {
        beerHeater = new ActuatorPwm(&defaultActuator, ccDefaults.heatPwmPeriod);
    }

    updateTemperatures();

    // Do not allow heating/cooling directly after reset.
    // A failing script + CRON + Arduino uno (which resets on serial connect) could damage the compressor
    // For test purposes, set these to -3600 to eliminate waiting after reset
    lastHeatTime = 0;
    lastCoolTime = 0;
}

void updateSensor(TempSensor * sensor)
{
    sensor -> update();

    if (!sensor -> isConnected())
    {
        sensor -> init();
    }
}

void TempControl::updateTemperatures(void)
{
    updateSensor(beerSensor);
    updateSensor(fridgeSensor);

    // Read ambient sensor to keep the value up to date. If no sensor is connected, this does nothing.
    // This prevents a delay in serial response because the value is not up to date.
    if (ambientSensor -> read() == TEMP_SENSOR_DISCONNECTED)
    {
        ambientSensor -> init();    // try to reconnect a disconnected, but installed sensor
    }
}

void TempControl::updatePID(void)
{
    static unsigned char integralUpdateCounter = 0;

    if (tempControl.modeIsBeer())
    {
        if (isDisabledOrInvalid(cs.beerSetting))
        {
            // beer setting is not updated yet
            // set fridge to unknown too
            cs.fridgeSetting = DISABLED_TEMP;

            return;
        }

        // fridge setting is calculated with PID algorithm. Beer temperature error is input to PID
        cv.beerDiff  = cs.beerSetting - beerSensor -> readSlowFiltered();
        cv.beerSlope = beerSensor -> readSlope();

        temperature fridgeFastFiltered = fridgeSensor -> readFastFiltered();

        if (integralUpdateCounter++ == 60)
        {
            integralUpdateCounter = 0;

            temperature integratorUpdate = cv.beerDiff;

            // Only update integrator in IDLE, because thats when the fridge temp has reached the fridge setting.
            // If the beer temp is still not correct, the fridge setting is too low/high and integrator action is needed.
            if (state != IDLE)
            {
                integratorUpdate = 0;
            }
            else if (abs(integratorUpdate) < cc.iMaxError)
            {
                // difference is smaller than iMaxError
                // check additional conditions to see if integrator should be active to prevent windup
                bool updateSign     = (integratorUpdate > 0);    // 1 = positive, 0 = negative
                bool integratorSign = (cv.diffIntegral > 0);

                if (updateSign == integratorSign)
                {
                    // beerDiff and integrator have same sign. Integrator would be increased.
                    // If actuator is already at max increasing actuator will only cause integrator windup.
                    integratorUpdate = (cs.fridgeSetting >= cc.tempSettingMax) ? 0 : integratorUpdate;
                    integratorUpdate = (cs.fridgeSetting <= cc.tempSettingMin) ? 0 : integratorUpdate;
                    integratorUpdate = ((cs.fridgeSetting - cs.beerSetting) >= cc.pidMax) ? 0 : integratorUpdate;
                    integratorUpdate = ((cs.beerSetting - cs.fridgeSetting) >= cc.pidMax) ? 0 : integratorUpdate;

                    // cooling and fridge temp is more than 2 degrees from setting, actuator is saturated.
                    integratorUpdate = (!updateSign && (fridgeFastFiltered > (cs.fridgeSetting + 1024)))
                                       ? 0 : integratorUpdate;

                    // heating and fridge temp is more than 2 degrees from setting, actuator is saturated.
                    integratorUpdate = (updateSign && (fridgeFastFiltered < (cs.fridgeSetting - 1024)))
                                       ? 0 : integratorUpdate;
                }
                else
                {
                    // integrator action is decreased. Decrease faster than increase.
                    integratorUpdate = integratorUpdate * 2;
                }
            }
            else
            {
                // decrease integral by 1/8 when far from the end value to reset the integrator
                integratorUpdate = -(cv.diffIntegral >> 3);
            }

            cv.diffIntegral = cv.diffIntegral + integratorUpdate;
        }

        // calculate PID parts. Use long_temperature to prevent overflow
        cv.p = multiplyFactorTemperatureDiff(cc.Kp, cv.beerDiff);
        cv.i = multiplyFactorTemperatureDiffLong(cc.Ki, cv.diffIntegral);
        cv.d = multiplyFactorTemperatureDiff(cc.Kd, cv.beerSlope);

        long_temperature newFridgeSetting = cs.beerSetting;

        newFridgeSetting += cv.p;
        newFridgeSetting += cv.i;
        newFridgeSetting += cv.d;

        // constrain to tempSettingMin or beerSetting - pidMAx, whichever is lower.
        temperature lowerBound = (cs.beerSetting <= cc.tempSettingMin + cc.pidMax)
                                 ? cc.tempSettingMin : cs.beerSetting - cc.pidMax;

        // constrain to tempSettingMax or beerSetting + pidMAx, whichever is higher.
        temperature upperBound = (cs.beerSetting >= cc.tempSettingMax - cc.pidMax)
                                 ? cc.tempSettingMax : cs.beerSetting + cc.pidMax;

        cs.fridgeSetting = constrainTemp(newFridgeSetting, lowerBound, upperBound);
    }
    else if (cs.mode == MODE_FRIDGE_CONSTANT)
    {
        // FridgeTemperature is set manually, disable beer setpoint
        cs.beerSetting = DISABLED_TEMP;
    }
}

void TempControl::updateState(void)
{
    // update state
    bool stayIdle    = false;
    bool newDoorOpen = door -> sense();

    if (newDoorOpen != doorOpen)
    {
        doorOpen = newDoorOpen;

        piLink.printFridgeAnnotation(PSTR("Fridge door %S"), doorOpen ? PSTR("opened") : PSTR("closed"));
    }

    if (cs.mode == MODE_OFF)
    {
        state    = STATE_OFF;
        stayIdle = true;
    }

    // stay idle when one of the required sensors is disconnected, or the fridge setting is INVALID_TEMP
    if (isDisabledOrInvalid(cs.fridgeSetting) ||!fridgeSensor -> isConnected()
            || (!beerSensor -> isConnected() && tempControl.modeIsBeer()))
    {
        state    = IDLE;
        stayIdle = true;
    }

    temperature     fridgeFast   = fridgeSensor -> readFastFiltered();
    temperature     beerFast     = beerSensor -> readFastFiltered();
    ticks_seconds_t secs         = ticks.seconds();

    switch (state)
    {
        case IDLE :
        case STATE_OFF :
        {
            lastIdleTime = secs;

            // set waitTime to zero. It will be set to the maximum required waitTime below when wait is in effect.
            if (stayIdle)
            {
                break;
            }

            if (fridgeFast > (cs.fridgeSetting + cc.idleRangeHigh))
            {    // fridge temperature is too high
                if (tempControl.chamberCooler -> getBareActuator() != &defaultActuator)
                {
                    state = COOLING;
                }
            }
            else if (fridgeFast < (cs.fridgeSetting + cc.idleRangeLow))
            {    // fridge temperature is too low
                if ((tempControl.chamberHeater -> getBareActuator() != &defaultActuator)
                        || (cc.lightAsHeater && (tempControl.light != &defaultActuator)))
                {
                    state = HEATING;
                }
            }
            else
            {
                state = IDLE;    // within IDLE range, always go to IDLE

                break;
            }
        }

        break;

        case COOLING :
        {
            if (tempControl.chamberCooler -> getBareActuator() == &defaultActuator)
            {
                state = IDLE;    // cooler uninstalled
                break;
            }
            lastCoolTime    = secs;
            state = COOLING;     // set to cooling here, so the display of COOLING/COOLING_MIN_TIME is correct
            // stop cooling when estimated fridge temp peak lands on target or if beer is already too cold (1/2 sensor bit idle zone)
            if (fridgeFast <= cs.fridgeSetting)
            {
                state = IDLE;
                break;
            }
        }

        break;

        case HEATING :
        {
            if (tempControl.chamberHeater -> getBareActuator() == &defaultActuator)
            {
                state = IDLE;    // heater uninstalled
                break;
            }
            if (fridgeFast >= cs.fridgeSetting)
            {
                state = IDLE;
                break;
            }
        }

        break;

        case DOOR_OPEN :
            break;    // do nothing
    }
}

void TempControl::updateOutputs(void)
{
    static long_temperature fridgeIntegrator = 0;
    if (cs.mode == MODE_TEST)
    {
        return;
    }

    cameraLight.update();

    bool heating = stateIsHeating();
    bool cooling = stateIsCooling();

    light -> setActive(isDoorOpen() || (cc.lightAsHeater && heating) || cameraLightState.isActive());
    fan -> setActive(heating || cooling);

    long_temperature proportionalPart;
    long_temperature integralPart;
    long_temperature dutyLong;
    long_temperature dutyConstrained;
    uint16_t duty;
    temperature fridgeError = cs.fridgeSetting - fridgeSensor -> readFastFiltered();
    temperature fridgeErrorForIntegral = constrainTemp(fridgeError, doubleToTempDiff(-1.0), doubleToTempDiff(1.0)); // prevent integral from growing too quicly

    long_temperature  antiWindup = 0;

    if (heating)
    {
    proportionalPart = multiplyFactorTemperatureDiff(cc.fridgePwmKpHeat / 4,
                fridgeError);    // returns -64/+64, divide by 4 to make it fit for now
        integralPart = multiplyFactorTemperatureDiff(cc.fridgePwmKiHeat,
                fridgeIntegrator / 240); // also divide by 60, same as with tempControl PID
        dutyLong = proportionalPart + integralPart;
        dutyConstrained = constrainTemp(dutyLong, 0, MAX_TEMP); // MAX_TEMP ~64
        duty = tempDiffToInt(4*dutyConstrained); // scale back to integer
        chamberHeater -> setPwm(duty);
        chamberCooler -> setPwm(0);
        antiWindup = dutyConstrained - dutyLong; // anti windup gain is 5* integral gain
        if(antiWindup > 0){
            antiWindup = 0;
        }
    }
    else if (cooling){
        proportionalPart = multiplyFactorTemperatureDiff(cc.fridgePwmKpCool / 4,
                fridgeError);    // returns -64/+64, divide by 4 to make it fit for now
        integralPart = multiplyFactorTemperatureDiff(cc.fridgePwmKiCool,
                fridgeIntegrator / 240); // also divide by 60, same as with tempControl PID
        dutyLong = proportionalPart + integralPart; // scale back
        dutyConstrained = constrainTemp(dutyLong, MIN_TEMP, 0); // MIN_TEMP ~ -64
        duty = -tempDiffToInt(4*dutyConstrained); // scale back to integer
        chamberCooler -> setPwm(duty);
        chamberHeater -> setPwm(0);
        long_temperature antiWindup = dutyConstrained - dutyLong; // anti windup gain is 5* proportional gain
        if(antiWindup < 0){
            antiWindup = 0;
        }
    }
    else
    {
        chamberHeater -> setPwm(0);
        chamberCooler -> setPwm(0);
    }
    fridgeIntegrator = fridgeIntegrator + fridgeErrorForIntegral + antiWindup;
}

void TempControl::updatePwm(void)
{
    chamberHeater -> updatePwm();
    chamberCooler -> updatePwm();
    beerHeater -> updatePwm();

}

tcduration_t TempControl::timeSinceCooling(void)
{
    return ticks.timeSince(lastCoolTime);
}

tcduration_t TempControl::timeSinceHeating(void)
{
    return ticks.timeSince(lastHeatTime);
}

tcduration_t TempControl::timeSinceIdle(void)
{
    return ticks.timeSince(lastIdleTime);
}

void TempControl::loadDefaultSettings()
{
#if BREWPI_EMULATE
    setMode(MODE_BEER_CONSTANT);
#else
    setMode(MODE_TEST);
#endif

    cs.beerSetting   = DISABLED_TEMP;            // start with no temp settings
    cs.fridgeSetting = DISABLED_TEMP;
}

void TempControl::storeConstants(eptr_t offset)
{
    eepromAccess.writeBlock(offset, (void *) &cc, sizeof(ControlConstants));
}

void TempControl::loadConstants(eptr_t offset)
{
    eepromAccess.readBlock((void *) &cc, offset, sizeof(ControlConstants));
    initFilters();
    chamberHeater -> setPeriod(cc.heatPwmPeriod);
    beerHeater -> setPeriod(cc.heatPwmPeriod);
    chamberCooler -> setPeriod(cc.coolPwmPeriod);
}

// write new settings to EEPROM to be able to reload them after a reset
// The update functions only write to EEPROM if the value has changed
void TempControl::storeSettings(eptr_t offset)
{
    eepromAccess.writeBlock(offset, (void *) &cs, sizeof(ControlSettings));

    storedBeerSetting = cs.beerSetting;
}

void TempControl::loadSettings(eptr_t offset)
{
    eepromAccess.readBlock((void *) &cs, offset, sizeof(ControlSettings));
    logDebug("loaded settings");

    storedBeerSetting = cs.beerSetting;

    setMode(cs.mode, true);    // force the mode update
}

void TempControl::loadDefaultConstants(void)
{
    memcpy_P((void *) &tempControl.cc, (void *) &ccDefaults, sizeof(ControlConstants));
    initFilters();
}

void TempControl::initFilters()
{
    fridgeSensor -> setFastFilterCoefficients(cc.fridgeFastFilter);
    fridgeSensor -> setSlowFilterCoefficients(cc.fridgeSlowFilter);
    fridgeSensor -> setSlopeFilterCoefficients(cc.fridgeSlopeFilter);
    beerSensor -> setFastFilterCoefficients(cc.beerFastFilter);
    beerSensor -> setSlowFilterCoefficients(cc.beerSlowFilter);
    beerSensor -> setSlopeFilterCoefficients(cc.beerSlopeFilter);
}

void TempControl::setMode(char newMode,
                          bool force)
{
    logDebug("TempControl::setMode from %c to %c", cs.mode, newMode);

    if ((newMode != cs.mode))
    {
        state = IDLE;
        force = true;
    }

    if (force)
    {
        cs.mode = newMode;

        if (newMode == MODE_OFF)
        {
            cs.beerSetting   = DISABLED_TEMP;
            cs.fridgeSetting = DISABLED_TEMP;
        }

        eepromManager.storeTempSettings();
    }
}

temperature TempControl::getBeerTemp(void)
{
    if (beerSensor -> isConnected())
    {
        return beerSensor -> readFastFiltered();
    }
    else
    {
        return INVALID_TEMP;
    }
}

temperature TempControl::getBeerSetting(void)
{
    return cs.beerSetting;
}

temperature TempControl::getFridgeTemp(void)
{
    if (fridgeSensor -> isConnected())
    {
        return fridgeSensor -> readFastFiltered();
    }
    else
    {
        return INVALID_TEMP;
    }
}

temperature TempControl::getFridgeSetting(void)
{
    return cs.fridgeSetting;
}

void TempControl::setBeerTemp(temperature newTemp)
{
    temperature oldBeerSetting = cs.beerSetting;

    cs.beerSetting = newTemp;

    updatePID();
    updateState();

    if ((cs.mode != MODE_BEER_PROFILE) || (abs(storedBeerSetting - newTemp) > intToTempDiff(1) / 4))
    {
        // more than 1/4 degree C difference with EEPROM
        // Do not store settings every time in profile mode, because EEPROM has limited number of write cycles.
        // A temperature ramp would cause a lot of writes
        // If Raspberry Pi is connected, it will update the settings anyway. This is just a safety feature.
        eepromManager.storeTempSettings();
    }
}

void TempControl::setFridgeTemp(temperature newTemp)
{
    cs.fridgeSetting = newTemp;

    updatePID();
    updateState();
    eepromManager.storeTempSettings();
}

bool TempControl::stateIsCooling(void)
{
    return state == COOLING;
}

bool TempControl::stateIsHeating(void)
{
    return state == HEATING;
}

control_mode_t ModeControl_GetMode()
{
    return tempControl.getMode();
}

control_mode_t ModeControl_SetMode(control_mode_t mode)
{
    control_mode_t prev = tempControl.getMode();

    tempControl.setMode(mode, true);

    return prev;
}
