// Copyright (c) makerspet.com 2024
// Apache-2.0 License
// Based on XV Lidar Controller v1.4.1
//   https://github.com/getSurreal/XV_Lidar_Controller

#include "xv_lib.h"

#define LDS_MOTOR_EN_PIN      19 // LDS enable pin
#define LDS_MOTOR_PWM_CHANNEL 2
#define LDS_MOTOR_PWM_BITS    11
#define LDS_MOTOR_PWM_HZ      30000

boolean ledState = LOW;
const int ledPin = 2;
const int SENSOR_TX = 17;
const int SENSOR_RX = 16;

void scan_callback(uint16_t, uint16_t, uint16_t, byte);
void motor_callback(int);
void packet_callback(uint16_t, byte*, uint16_t);

XV xv_lds;

void setup() {
  Serial.begin(115200); // USB serial

  pinMode(LDS_MOTOR_EN_PIN, OUTPUT);
  pinMode(ledPin, OUTPUT);

  ledcSetup(LDS_MOTOR_PWM_CHANNEL, LDS_MOTOR_PWM_HZ, LDS_MOTOR_PWM_BITS);
  ledcAttachPin(LDS_MOTOR_EN_PIN, LDS_MOTOR_PWM_CHANNEL);

  // HardwareSerial LdSerial(2); // TX 17, RX 16
  Serial1.begin(115200, SERIAL_8N1, SENSOR_RX, SENSOR_TX); // XV LDS data

  xv_lds.setScanPointCallback(xv_scan_callback);
  xv_lds.setMotorCallback(xv_motor_callback);
  xv_lds.setPacketCallback(xv_packet_callback);  
  xv_lds.enableMotor(true);
}

void loop() {
  while (Serial1.available() > 0) { // read byte from LDS
    xv_lds.processByte(Serial1.read());
  }
  if (!xv_lds.loop()) {
    // LDS motor error
    //Serial.println("LDS motor error");
    //xv_lds.enableMotor(false);
  }
}

void xv_scan_callback(uint16_t angle_deg, uint16_t distance_mm,
  uint16_t quality, byte err) {
  //Serial.print(angle_deg);
  //Serial.print(" ");
  //Serial.print(distance_mm);
  //Serial.print(" ");
  //Serial.print(quality);
  //Serial.print(" ");
  //Serial.println(err);
}

void xv_motor_callback(float pwm) {
  //Serial.print("Motor callback ");
  //Serial.print(xv_lds.getMotorRPM());
  //Serial.print(" ");
  //Serial.println(pwm);
  //Serial.print(" ");
  int pwm_value = ((1<<LDS_MOTOR_PWM_BITS)-1)*pwm;
  ledcWrite(LDS_MOTOR_PWM_CHANNEL, pwm_value);
  //Serial.println(pwm_value);
}

void xv_packet_callback(uint16_t starting_angle_deg, byte* packet, uint16_t length) {    
  //Serial.print(starting_angle_deg);
  //Serial.print(" ");
  //Serial.print(length);
  //Serial.print(" ");
  //Serial.println(xv_lds.getMotorRPM());
  Serial.write(packet, length); // dump raw data
}
