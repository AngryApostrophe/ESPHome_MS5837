#include "esphome.h"
#include "Arduino.h"
#include <Wire.h>

#define MS5837_ADDR 0x76  //I2C address of the sensor

#define MS5837_RESET 0x1E
#define MS5837_ADC_READ 0x00
#define MS5837_PROM_READ 0xA0
#define MS5837_CONVERT_D1_8192 0x4A
#define MS5837_CONVERT_D2_8192 0x5A

#define MS5837_VERSION_02BA01 0x00 // Sensor version: From MS5837_02BA datasheet Version PROM Word 0
#define MS5837_VERSION_02BA21 0x15 // Sensor version: From MS5837_02BA datasheet Version PROM Word 0
#define MS5837_VERSION_30BA26 0x1A // Sensor version: From MS5837_30BA datasheet Version PROM Word 0
#define MS5837_VERSION_02BA06 0x5D // Sensor version: From actual device.  Doesn't seem to be mentioned in the datasheet
#define MS5837_VERSION_UNKNOWN 0xFF

#define MS5837_ISA_SEALEVEL_PRESSURE   101325   // (Pa)

#define MS5837_MODE_RAW        0    // No altitude/depth
#define MS5837_MODE_ALTITUDE   1    // Return either true altitude or pressure altitude depending on if user supplies a sea level pressure
#define MS5837_MODE_DEPTH      2    // Return depth.  Needs ambient pressure for accurate results (see service "update_pressure")

#define MS5837_UNITS_ALT_M         0
#define MS5837_UNITS_ALT_FT        1
#define MS5837_UNITS_ALT_CM        2
#define MS5837_UNITS_ALT_IN        3
#define MS5837_UNITS_PRESS_HPA     0
#define MS5837_UNITS_PRESS_PA      1
#define MS5837_UNITS_PRESS_KPA     2
#define MS5837_UNITS_PRESS_INHG    3
#define MS5837_UNITS_TEMP_C        0
#define MS5837_UNITS_TEMP_F        1
#define MS5837_UNITS_TEMP_K        2
#define MS5837_UNITS_TEMP_R        3


class MS5837_Component : public PollingComponent, public Sensor, public CustomAPIDevice 
{
public:

	Sensor *temperature_sensor = new Sensor();
	Sensor *pressure_sensor = new Sensor();
	Sensor *altitude_sensor = new Sensor(); //This will also be depth, if in MS5837_MODE_DEPTH

	MS5837_Component(uint32_t Updatems = 60000, uint8_t mode = MS5837_MODE_RAW) : PollingComponent(Updatems)
	{
		bInitialized = false;
		fluidDensity = 997; //Density of fresh water.  User can change this with SetDensity()
		atmosphericpress = MS5837_ISA_SEALEVEL_PRESSURE; //ISA sea level pressure
		bPCalcMode = mode;

		//Setup default units if user doesn't specify
			bAltUnits = MS5837_UNITS_ALT_M;
			bPressUnits = MS5837_UNITS_PRESS_HPA;
			bTempUnits = MS5837_UNITS_TEMP_C;
	}

	float get_setup_priority() const override { return esphome::setup_priority::BUS; }

	void setup() override
	{
		ESP_LOGI("custom", "MS5837:  Initializing MS5837");

		uint32_t err;

		//Register a service with Home Assistant so we can update the atmospheric pressure for depth calculations
			register_service(&MS5837_Component::on_update_pressure, "update_pressure", {"New Pressure (hPa)"});

		// Reset the MS5837, per datasheet
			Wire.beginTransmission(MS5837_ADDR);
			Wire.write(MS5837_RESET);
			err = Wire.endTransmission();
			if (err != 0) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error resetting device.  Error no %d.", err);
				return;
			}

		// Wait for reset to complete
			delay(10);

		// Read calibration values and CRC
			for ( uint8_t i = 0 ; i < 7 ; i++ ) 
			{
				Wire.beginTransmission(MS5837_ADDR);
				Wire.write(MS5837_PROM_READ+i*2);
				err = Wire.endTransmission();
				if (err != 0) 
				{
					ESP_LOGE("custom", "MS5837:  Comms error reading calibration values  Error no %d.", err);
					return;
				}

				err = Wire.requestFrom(MS5837_ADDR,2);
				if (err != 2)
				{
					ESP_LOGE("custom", "MS5837:  Comms error reading calibration values.  Received invalid response.");
					return;
				}

				C[i] = (Wire.read() << 8) | Wire.read();
			}

		// Verify that data is correct with CRC
			uint8_t crcRead = C[0] >> 12;
			uint8_t crcCalculated = crc4(C);

			if ( crcCalculated != crcRead ) 
			{
				ESP_LOGE("custom", "MS5837:  Initialization failed.  CRC error.  Error no %d.", err);
				return;
			}

		// Set _model according to the sensor version
			uint8_t version = (C[0] >> 5) & 0x7F; // Extract the sensor version from PROM Word 0

			if (version == MS5837_VERSION_02BA01 || version == MS5837_VERSION_02BA21 || version == MS5837_VERSION_30BA26 || version == MS5837_VERSION_02BA06)
			{
				_model = version;
			}
			else
			{
				ESP_LOGW("custom", "MS5837:  Unknown sensor model:  0x%02X.  Will continue but results may be incorrect.", version);
				_model = MS5837_VERSION_UNKNOWN;
			}

		bInitialized = true;
		ESP_LOGI("custom", "MS5837:  MS5837 initialized.");
	}
    
	void update()
	{
		uint32_t err;

		//Check that the sensor has been initialized.  If not, try it
			if (!bInitialized)
				setup();

		//We tried but it failed.  Abort
			if (!bInitialized)
			{
				InvalidateSensors();
				return;
			}

		// Request D1 (raw pressure)
			Wire.beginTransmission(MS5837_ADDR);
			Wire.write(MS5837_CONVERT_D1_8192);
			err = Wire.endTransmission(); 
			if (err != 0) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error requesting D1.  Error no %d.", err);
				InvalidateSensors();
				return;
			}

			delay(20); // Max conversion time per datasheet

		//Read the response
			Wire.beginTransmission(MS5837_ADDR);
			Wire.write(MS5837_ADC_READ);
			err = Wire.endTransmission(); 
			if (err != 0) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error requesting D1 Read.  Error no %d.", err);
				InvalidateSensors();
				return;
			}

			err = Wire.requestFrom(MS5837_ADDR,3);
			if (err != 3) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error reading D1 reply.  Received invalid response.");
				InvalidateSensors();
				return;
			}

			D1_pres = 0;
			D1_pres = Wire.read();
			D1_pres = (D1_pres << 8) | Wire.read();
			D1_pres = (D1_pres << 8) | Wire.read();

		// Request D2 (raw temperature)
			Wire.beginTransmission(MS5837_ADDR);
			Wire.write(MS5837_CONVERT_D2_8192);
			err = Wire.endTransmission(); 
			if (err != 0) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error requesting D2.  Error no %d.", err);
				InvalidateSensors();
				return;
			}

			delay(20); // Max conversion time per datasheet

		//Read the response
			Wire.beginTransmission(MS5837_ADDR);
			Wire.write(MS5837_ADC_READ);
			err = Wire.endTransmission(); 
			if (err != 0) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error requesting D2 Read.  Error no %d.", err);
				InvalidateSensors();
				return;
			}

			err = Wire.requestFrom(MS5837_ADDR,3);
			if (err != 3) 
			{
				ESP_LOGE("custom", "MS5837:  Comms error reading D2 reply.  Received invalid response.");
				InvalidateSensors();
				return;
			}

			D2_temp = 0;
			D2_temp = Wire.read();
			D2_temp = (D2_temp << 8) | Wire.read();
			D2_temp = (D2_temp << 8) | Wire.read();

		//Calculate the values
			calculate();

		//Publish the results
			pressure_sensor->publish_state(ConvertPress(Pressure()));
			temperature_sensor->publish_state(ConvertTemp(Temperature()));

			if (bPCalcMode == MS5837_MODE_ALTITUDE)
				altitude_sensor->publish_state(ConvertAlt(Altitude()));
			else if (bPCalcMode == MS5837_MODE_DEPTH)    
				altitude_sensor->publish_state(ConvertAlt(Depth()));
	}
    
	//Run the calculations from the raw temp/press reported by the sensor
	void calculate() 
	{
		// Given C1-C6 and D1, D2, calculated TEMP and P
		// Do conversion first and then second order temp compensation

		int32_t dT = 0;
		int64_t SENS = 0;
		int64_t OFF = 0;
		int32_t SENSi = 0;
		int32_t OFFi = 0;
		int32_t Ti = 0;
		int64_t OFF2 = 0;
		int64_t SENS2 = 0;

		// Terms called
			dT = D2_temp-uint32_t(C[5])*256l;
			if (  _model == MS5837_VERSION_02BA01 || _model == MS5837_VERSION_02BA21 || _model == MS5837_VERSION_02BA06) 
			{
				SENS = int64_t(C[1])*65536l+(int64_t(C[3])*dT)/128l;
				OFF = int64_t(C[2])*131072l+(int64_t(C[4])*dT)/64l;
				P = (D1_pres*SENS/(2097152l)-OFF)/(32768l);
			} 
			else 
			{
				SENS = int64_t(C[1])*32768l+(int64_t(C[3])*dT)/256l;
				OFF = int64_t(C[2])*65536l+(int64_t(C[4])*dT)/128l;
				P = (D1_pres*SENS/(2097152l)-OFF)/(8192l);
			}

		// Temp conversion
			TEMP = 2000l+int64_t(dT)*C[6]/8388608LL;

		//Second order compensation
			if ( _model == MS5837_VERSION_02BA01 || _model == MS5837_VERSION_02BA21 || _model == MS5837_VERSION_02BA06) 
			{
				if((TEMP/100)<20) //Low temp
				{         
					Ti = (11*int64_t(dT)*int64_t(dT))/(34359738368LL);
					OFFi = (31*(TEMP-2000)*(TEMP-2000))/8;
					SENSi = (63*(TEMP-2000)*(TEMP-2000))/32;
				}
			} 
			else 
			{
				if((TEMP/100)<20) //Low temp
				{         
					Ti = (3*int64_t(dT)*int64_t(dT))/(8589934592LL);
					OFFi = (3*(TEMP-2000)*(TEMP-2000))/2;
					SENSi = (5*(TEMP-2000)*(TEMP-2000))/8;
					if((TEMP/100)<-15)  //Very low temp
					{   
						OFFi = OFFi+7*(TEMP+1500l)*(TEMP+1500l);
						SENSi = SENSi+4*(TEMP+1500l)*(TEMP+1500l);
					}
				}
				else if((TEMP/100)>=20) //High temp
				{    
					Ti = 2*(dT*dT)/(137438953472LL);
					OFFi = (1*(TEMP-2000)*(TEMP-2000))/16;
					SENSi = 0;
				}
			}

		//Calculate pressure and temp second order
			OFF2 = OFF-OFFi; 
			SENS2 = SENS-SENSi;

			TEMP = (TEMP-Ti);

			if ( _model == MS5837_VERSION_02BA01 || _model == MS5837_VERSION_02BA21 || _model == MS5837_VERSION_02BA06) 
			{
				P = (((D1_pres*SENS2)/2097152l-OFF2)/32768l);
			} 
			else 
			{
				P = (((D1_pres*SENS2)/2097152l-OFF2)/8192l);
			}
	}

	//Home Assistant has sent us a new atmospheric pressure for depth calculations.  Must be in hPa
		void on_update_pressure(std::string pressure)
		{
			float num_float = std::stof(pressure);

			//Sanity check on the new pressure
				if (num_float > 1200 || num_float < 200)
				{
					ESP_LOGE("custom", "MS5837:  Received invalid atmospheric pressure from Home Assistant. Ignoring.  Current pressure: %0.2f hPa", atmosphericpress/100.0f);
					return;
				}

			num_float *= 100.0f; //hPa to Pa
			atmosphericpress = num_float;

			ESP_LOGI("custom", "MS5837:  Atmospheric Pressure updated to %0.2f hPa", num_float/100.0f);
		}
    
	//Allow user to change target units from ESPHome device yaml
		void SetUnits(uint8_t bAlt, uint8_t bPress, uint8_t bTemp)
		{
			bAltUnits = bAlt;
			bPressUnits = bPress;
			bTempUnits = bTemp;
		}

	//Allow the user to change water density (kg/m^3), such as when they're putting the sensor in salt water (1029 kg/m^3)
		void SetDensity(float fDensity)
		{
			fluidDensity = fDensity;
		}
    
private:
	uint8_t _model; //Sensor model as read from the device
	bool bInitialized;
	uint8_t bPCalcMode; // 0=Raw data only.  1=Altitude.  2=Depth

	//Units in which to report the results
		uint8_t bAltUnits;
		uint8_t bPressUnits;
		uint8_t bTempUnits;

	float fluidDensity; 			//Density of the fluid the sensor is in (kg/m^3).  Used for converting pressure to depth.
	float atmosphericpress; 	//This has 2 uses.  In MS5837_MODE_ALTITUDE it is the Sea Level Pressure in Pa, if known (for example, from a weather station).  You'll get more accurate height results.  If not known, it'll use ISA (101325 pa)
														//                  In MS5837_MODE_DEPTH it is the actual ambient atmospheric pressure in Pa.

	//Calculation variables
		uint16_t C[8];
		uint32_t D1_pres, D2_temp;
		int32_t TEMP;
		int32_t P;
	
	uint8_t crc4(uint16_t n_prom[]) 
	{
    	uint16_t n_rem = 0;
    
    	n_prom[0] = ((n_prom[0]) & 0x0FFF);
    	n_prom[7] = 0;
    
    	for ( uint8_t i = 0 ; i < 16; i++ ) {
    		if ( i%2 == 1 ) {
    			n_rem ^= (uint16_t)((n_prom[i>>1]) & 0x00FF);
    		} else {
    			n_rem ^= (uint16_t)(n_prom[i>>1] >> 8);
    		}
    		for ( uint8_t n_bit = 8 ; n_bit > 0 ; n_bit-- ) {
    			if ( n_rem & 0x8000 ) {
    				n_rem = (n_rem << 1) ^ 0x3000;
    			} else {
    				n_rem = (n_rem << 1);
    			}
    		}
    	}
    
    	n_rem = ((n_rem >> 12) & 0x000F);
    
    	return n_rem ^ 0x00;
    }
    
	//Pressure returned in mbar or mbar*conversion rate.
		float Pressure(float conversion = 1.0f) 
		{
			if ( _model == MS5837_VERSION_02BA01 || _model == MS5837_VERSION_02BA21 || _model == MS5837_VERSION_02BA06) 
				return P*conversion/100.0f;
			else 
				return P*conversion/10.0f;
		}
    
	//Temperature returned in deg C.
		float Temperature()
		{
			return TEMP/100.0f;
		}

	//Calculate the depth, given the sensed pressure and known ambient pressure
		float Depth() 
		{
			// The pressure sensor measures absolute pressure, so it will measure the atmospheric pressure + water pressure
			// We subtract the atmospheric pressure to calculate the depth with only the water pressure
			return (Pressure(100)-atmosphericpress)/(fluidDensity*9.80665);  //Pressure(100) converts hPa to Pa
		}

	//Calculate Pressure Altitude in m (relative to sea level at ISA)
		float Altitude() 
		{
			return (1-pow((Pressure()/(atmosphericpress/100)),.190284))*145366.45*.3048;
		}
    
	//If an update fails, invalidate all the sensors. ("Unknown" in Home Assistant)
		void InvalidateSensors()
		{
			pressure_sensor->publish_state(NAN);
			temperature_sensor->publish_state(NAN);

			if (bPCalcMode == MS5837_MODE_ALTITUDE || bPCalcMode == MS5837_MODE_DEPTH)
				altitude_sensor->publish_state(NAN);
		}

	//Convert the altitude output from meters to the user's chosen units
		float ConvertAlt(float fAlt)
		{
			//Always comes in as Meters

			if (bAltUnits == MS5837_UNITS_ALT_FT)
				fAlt *= 3.28084f;
			else if (bAltUnits == MS5837_UNITS_ALT_CM)
				fAlt *= 100;
			else if (bAltUnits == MS5837_UNITS_ALT_IN)
				fAlt = (fAlt*3.28084f) * 12;

			return fAlt;
		}
    
	//Convert the temperature output from °C to the user's chosen units
		float ConvertTemp(float fTemp)
		{
			//Always comes in as C

			if (bTempUnits == MS5837_UNITS_TEMP_F)
				fTemp = (fTemp * (9.0f/5.0f)) + 32;
			else if (bTempUnits == MS5837_UNITS_TEMP_K)
				fTemp += 273.15;
			else if (bTempUnits == MS5837_UNITS_TEMP_R)
				fTemp += ((fTemp * (9.0f/5.0f)) + 32) + 459.67;

			return fTemp;
		}

	//Convert the pressure output from hPa to the user's chosen units
		float ConvertPress(float fPress)
		{
			//Always comes in as hPa

			if (bPressUnits == MS5837_UNITS_PRESS_PA)
				fPress *= 100;
			else if (bPressUnits == MS5837_UNITS_PRESS_KPA)
				fPress /= 10;
			else if (bPressUnits == MS5837_UNITS_PRESS_INHG)
				fPress *= 0.02952998330101;

			return fPress;
		}
};
