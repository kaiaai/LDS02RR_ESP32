// Copyright (c) 2024 makerspet.com
//   Apache 2.0 license
// Based on
//   Copyright 2014-2021 James LeRoy getSurreal.com
//   https://github.com/getSurreal/XV_Lidar_Controller

#include "xv_lib.h"

XV::XV() {
  scan_callback = NULL;
  motor_callback = NULL;
  packet_callback = NULL;

  eState = eState_Find_COMMAND;
  ixPacket = 0;                          // index into 'Packet' array
  motor_enabled = false;

  scan_rpm_min = scan_rpm_setpoint*0.8;
  scan_rpm_max = scan_rpm_setpoint*1.1;
  pwm_val = 0.5;

  //rpm_setpoint = 0;
  scan_rpm_setpoint = DEFAULT_SCAN_RPM;  // desired RPM 1.8KHz/5FPS/360 = 1 deg resolution
  scanRpmPID.init(&scan_rpm, &pwm_val, &scan_rpm_setpoint, 3.0e-3, 1.0e-3, 0.0, scanRpmPID.DIRECT);
  scanRpmPID.SetOutputLimits(0, 1.0);
  scanRpmPID.SetSampleTime(20);
  scanRpmPID.SetMode(scanRpmPID.AUTOMATIC);

  ClearVars();

  scan_period_ms = 0;

  motor_check_timer = millis();
  motor_check_interval = 200;
  scan_rpm_err_thresh = 10;  // 2 seconds (10 * 200ms) to shutdown motor with improper RPM and high voltage
  scan_rpm_err = 0;
  lastMillis = millis();
}

void XV::setPacketCallback(PacketCallback packet_callback) {
  this->packet_callback = packet_callback;
}

void XV::setMotorPwmCallback(MotorPwmCallback motor_callback) {
  this->motor_callback = motor_callback;
}

void XV::setScanPointCallback(ScanPointCallback scan_callback) {
  this->scan_callback = scan_callback; 
}

bool XV::setScanRPM(float rpm) {
  scan_rpm_setpoint = (rpm <= 0) ? DEFAULT_SCAN_RPM : rpm;
  return true;
}

void XV::setScanRpmPIDCoeffs(float Kp, float Ki, float Kd) {
  scanRpmPID.SetTunings(Kp, Ki, Kd);
}

void XV::setScanRpmPIDSamplePeriod(int sample_period_ms) {
  scanRpmPID.SetSampleTime(sample_period_ms);
}

void XV::processByte(int inByte) {
  // Switch, based on 'eState':
  // State 1: We're scanning for 0xFA (COMMAND) in the input stream
  // State 2: Build a complete data packet
  if (eState == eState_Find_COMMAND) {      // flush input until we get COMMAND byte
    if (inByte == COMMAND) {
      eState++;                                 // switch to 'build a packet' state
      Packet[ixPacket++] = inByte;              // store 1st byte of data into 'Packet'
    }
  } else {
    Packet[ixPacket++] = inByte;        // keep storing input into 'Packet'
    if (ixPacket == PACKET_LENGTH) {
      // we've got all the input bytes, so we're done building this packet
      
      if (IsValidPacket()) {      // Check packet CRC
        byte aryInvalidDataFlag[N_DATA_QUADS] = {0, 0, 0, 0}; // non-zero = INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set

        // the first scan angle (of group of 4, based on 'index'), in degrees (0..359)
        // get the starting angle of this group (of 4), e.g., 0, 4, 8, 12, ...
        uint16_t startingAngle = ProcessIndex();

        if (packet_callback)
          packet_callback(startingAngle, Packet, PACKET_LENGTH);

        ProcessSpeed();

        // process each of the (4) sets of data in the packet
        for (int ix = 0; ix < N_DATA_QUADS; ix++)   // process the distance
          aryInvalidDataFlag[ix] = ProcessDistance(ix);
        for (int ix = 0; ix < N_DATA_QUADS; ix++) { // process the signal strength (quality)
          aryQuality[ix] = 0;
          if (aryInvalidDataFlag[ix] == 0)
            ProcessSignalStrength(ix);
        }

        for (int ix = 0; ix < N_DATA_QUADS; ix++) {
          byte err = aryInvalidDataFlag[ix] & BAD_DATA_MASK;
          if (scan_callback)
            scan_callback(startingAngle + ix, int(aryDist[ix]), aryQuality[ix], err);
        }

      } else {
        // Bad packet
        if (packet_callback)
          packet_callback(0, 0, 0);
      }

      ClearVars();   // initialize a bunch of stuff before we switch back to State 1
    }
  }
}

void XV::ClearVars() {
  for (int ix = 0; ix < N_DATA_QUADS; ix++) {
    aryDist[ix] = 0;
    aryQuality[ix] = 0;
    //aryInvalidDataFlag[ix] = 0;
  }
  for (ixPacket = 0; ixPacket < PACKET_LENGTH; ixPacket++)  // clear out this packet
    Packet[ixPacket] = 0;
  ixPacket = 0;
  eState = eState_Find_COMMAND; // This packet is done -- look for next COMMAND byte
}


/*
   ProcessIndex - Process the packet element 'index'
   index is the index byte in the 90 packets, going from A0 (packet 0, readings 0 to 3) to F9
      (packet 89, readings 356 to 359).
   Uses:       Packet
               curMillis = milliseconds, now
               lastMillis = milliseconds, last time through this subroutine
   Returns:    The first angle (of 4) in the current 'index' group
*/
uint16_t XV::ProcessIndex() {
  uint16_t angle = 0;
  uint16_t data_4deg_index = Packet[OFFSET_TO_INDEX] - INDEX_LO;
  angle = data_4deg_index * N_DATA_QUADS;     // 1st angle in the set of 4

  if (angle == 0) {
    unsigned long curMillis = millis();
    // Time Interval in ms since last complete revolution
    scan_period_ms = curMillis - lastMillis;
    lastMillis = curMillis;
  }
  return angle;
}

int XV::lastScanPeriodMs() {
  return scan_period_ms;
}

void XV::ProcessSpeed() {
  // Extract motor speed from packet - two bytes little-endian, equals RPM/64
  uint8_t scan_rph_low_byte = Packet[OFFSET_TO_SPEED_LSB];
  uint8_t scan_rph_high_byte = Packet[OFFSET_TO_SPEED_MSB];
  scan_rpm = float( (scan_rph_high_byte << 8) | scan_rph_low_byte ) / 64.0;
}

/*
   Data 0 to Data 3 are the 4 readings. Each one is 4 bytes long, and organized as follows :
     byte 0 : <distance 7:0>
     byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8>
     byte 2 : <signal strength 7:0>
     byte 3 : <signal strength 15:8>
*/
/*
   ProcessDistance- Process the packet element 'distance'
   Enter with: iQuad = which one of the (4) readings to process, value = 0..3
   Uses:       Packet
               dist[] = sets distance to object in binary: ISbb bbbb bbbb bbbb
                                       so maximum distance is 0x3FFF (16383 decimal) millimeters (mm)
   Exits with: 0 = okay
   Error:      1 << 7 = INVALID_DATA_FLAG is set
               1 << 6 = STRENGTH_WARNING_FLAG is set
*/
byte XV::ProcessDistance(int iQuad) {
  uint8_t dataL, dataM;
  aryDist[iQuad] = 0;                     // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_DISTANCE_LSB;
  // byte 0 : <distance 7:0> (LSB)
  // byte 1 : <"invalid data" flag> <"strength warning" flag> <distance 13:8> (MSB)
  dataM = Packet[iOffset + 1];           // get MSB of distance data + flags
  if (dataM & BAD_DATA_MASK)             // if either INVALID_DATA_FLAG or STRENGTH_WARNING_FLAG is set...
    return dataM & BAD_DATA_MASK;        // ...then return non-zero
  dataL = Packet[iOffset];               // LSB of distance data
  aryDist[iQuad] = dataL | ((dataM & 0x3F) << 8);
  return 0;                              // okay
}

/*
   ProcessSignalStrength- Process the packet element 'signal strength'
   Enter with: iQuad = which one of the (4) readings to process, value = 0..3
   Uses:       Packet
               quality[] = signal quality
*/
void XV::ProcessSignalStrength(int iQuad) {
  uint8_t dataL, dataM;
  aryQuality[iQuad] = 0;                        // initialize
  int iOffset = OFFSET_TO_4_DATA_READINGS + (iQuad * N_DATA_QUADS) + OFFSET_DATA_SIGNAL_LSB;
  dataL = Packet[iOffset];                  // signal strength LSB
  dataM = Packet[iOffset + 1];
  aryQuality[iQuad] = dataL | (dataM << 8);
}

/*
   ValidatePacket - Validate 'Packet'
   Enter with: 'Packet' is ready to check
   Uses:       CalcCRC
   Exits with: 0 = Packet is okay
   Error:      non-zero = Packet is no good
*/
bool XV::IsValidPacket() {
  unsigned long chk32;
  unsigned long checksum;
  const int bytesToCheck = PACKET_LENGTH - 2;
  const int CalcCRC_Len = bytesToCheck / 2;
  unsigned int CalcCRC[CalcCRC_Len];

  byte b1a, b1b, b2a, b2b;
  int ix;

  for (int ix = 0; ix < CalcCRC_Len; ix++)       // initialize 'CalcCRC' array
    CalcCRC[ix] = 0;

  // Perform checksum validity test
  for (ix = 0; ix < bytesToCheck; ix += 2)      // build 'CalcCRC' array
    CalcCRC[ix / 2] = Packet[ix] + ((Packet[ix + 1]) << 8);

  chk32 = 0;
  for (ix = 0; ix < CalcCRC_Len; ix++)
    chk32 = (chk32 << 1) + CalcCRC[ix];
  checksum = (chk32 & 0x7FFF) + (chk32 >> 15);
  checksum &= 0x7FFF;
  b1a = checksum & 0xFF;
  b1b = Packet[OFFSET_TO_CRC_L];
  b2a = checksum >> 8;
  b2b = Packet[OFFSET_TO_CRC_M];

  return ((b1a == b1b) && (b2a == b2b));
}

bool XV::loop() {
  if (!motor_enabled)
    return false;

  scanRpmPID.Compute();
  if (pwm_val != pwm_last) {
    if (motor_callback)
      motor_callback(float(pwm_val));
    pwm_last = pwm_val;
  }
  return motorCheck();
}

void XV::enableMotor(bool enable) {
  motor_enabled = enable;
  
  if (enable)
    scan_rpm_err = 0;  // reset rpm error

  if (motor_callback)
    motor_callback(enable ? float(pwm_val) : 0);
}

bool XV::motorCheck() {  // Make sure the motor RPMs are good else shut it down
  unsigned long now = millis();
  if (now - motor_check_timer <= motor_check_interval)
    return false;
  
  if (motor_enabled && ((scan_rpm < scan_rpm_min) || (scan_rpm > scan_rpm_max))) {
    scan_rpm_err++;
  } else {
    scan_rpm_err = 0;
  }

  motor_check_timer = millis();

  // TODO instead, check how long the RPM has been out of bounds
  return (scan_rpm_err > scan_rpm_err_thresh);
}

float XV::getScanRPM() {
  return scan_rpm;
}

bool XV::isMotorEnabled() {
  return motor_enabled;
}
