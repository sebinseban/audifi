
//===================================================================//
//
//  -- AUDIFI Receiver --
//
//  The receiver end code for AUDIFI.
//
//  Version 0.3
//  Last modified : +05:30 15:10:45 PM 13-07-2021, Tuesday
//  
//===================================================================//

// #include <ESP8266WiFi.h>
#include <WiFi.h>
// #include "esp_wifi.h"
#include <WiFiUdp.h>
#include "ptScheduler.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

//===================================================================//

//WiFi credentials of the server
#define WIFI_SSID "AUDIFI-SERVER"
#define WIFI_PASS "12345678"
#define UDP_PORT 4210
#define DEBUG_LED 2
// #define REQUEST_SIZE 22054  //size required for data
// #define REQUEST_SIZE 11029  //size required for data
#define REQUEST_SIZE 11029  //size required for data
#define REQUEST_HEADER_SIZE 4 //size required for buffer header info

#define CB_SIZE 110250

//ESP32 pins
#define PIN_LEFT_CHANNEL 13
#define PIN_RIGHT_CHANNEL 4

#define debugSerial Serial
#define dataSerial Serial2

#define DEBUG_SERIAL_BAUDRATE 500000

#define UDP_MTU_SIZE 1460

//this is to make it easy to deifine a new circular buffer
#define CIRC_BBUF_DEF(x,y)                \
    uint8_t x##_data_space[y];            \
    circ_buf_t x = {                      \
        .buffer = x##_data_space,         \
        .head = 0,                        \
        .tail = 0,                        \
        .maxlen = y                       \
    }

//===================================================================//

//Circular Buffer struct.
typedef struct {
    uint8_t* buffer;
    int head;
    int tail;
    int maxlen;
} circ_buf_t;

circ_buf_t audioBuffer;

bool audioBufferEmpty = true;

//UDP parameters
WiFiUDP UDP;
IPAddress server_IP (192,168,4,1);
IPAddress client_IP (192,168,4,2);

//this is a temp buffer used during UDP data transfer.
//this is required because the UDP RX buffer may be reused for receiving fragmented data.
//the data will be assmebled in the below buffer before it is copied to the next
//audio data buffer.
uint16_t tempAudioBufferLength = 0;
uint8_t tempAudioBuffer[REQUEST_SIZE] = {0};

//periodic tasks
ptScheduler pwmTask = ptScheduler(100);
ptScheduler ledTask = ptScheduler(500000);
ptScheduler dataRequestTask = ptScheduler(2000000);

//PWM parameters
const int pwmFrequency = 22000;
const int leftChannel = 0;  //PWM channel
const int rightChannel = 1; //PWM channel
uint8_t leftDutycycle = 0;
uint8_t rightDutycycle = 0;
const int pwmResolution = 8;

volatile int interruptCounter = 0;
int totalInterruptCounter = 0;
int totalServedInterruptCounter = 0;
 
hw_timer_t* timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE bufferSignalMux = portMUX_INITIALIZER_UNLOCKED;

bool serverReady = false;
bool dataRequestMade = false;
bool dataRequestFulfilled = false;

int udpRxPacketSize = 0;
int udpRxDataLength = 0;
char udpRxDataBuffer[UDP_MTU_SIZE] = {0};

//===================================================================//

int requestData (int length = REQUEST_SIZE);

//===================================================================//
//This function is invoked at a frequency of 44.1Khz.
//It increments a counter indicating how many interrupts have been
//occured.

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);  //lock the mutex
  interruptCounter++;
  totalInterruptCounter++;
  portEXIT_CRITICAL_ISR(&timerMux); //unlock the mutex
}

//===================================================================//
//This function checks if the 44.1Khz timer was interrupted.
//If it was interrupted, send a single sample to both L and R PWM pins.
//The interrupt counter may have a value more than 1. In such case,
//transmit samples of that many.

void streamTask(void* pvParameters) {
  while(1) {
    if (interruptCounter > 0) { //if the timer was interrupted
      //start reading the CB only if it is not empty.
      if (!audioBufferEmpty) {
        uint8_t sampleByte = 0;
        int cbState = cbPop(&audioBuffer, &sampleByte);
        
        if (cbState == 0) {
          ledcWrite(rightChannel, sampleByte);
        }
        else {
          portENTER_CRITICAL(&bufferSignalMux);
            audioBufferEmpty = true;
          portEXIT_CRITICAL(&bufferSignalMux);
          ledcWrite(rightChannel, 0);
        }
      }
      portENTER_CRITICAL(&timerMux);
        interruptCounter = 0;
      portEXIT_CRITICAL(&timerMux);
    }
    wdtFeed();
  }
}

//===================================================================//

void setup() {
  // quadBuffer = (uint8_t*) malloc( sizeof(uint8_t) * BUFFER_COUNT * REQUEST_SIZE);

  // if (quadBuffer == NULL) {
  //   debugSerial.println("Memory allocation failed");
  // }
  audioBuffer.head = 0;
  audioBuffer.tail = 0;
  audioBuffer.maxlen = (CB_SIZE+1);
  audioBuffer.buffer = (uint8_t*) malloc(sizeof(uint8_t) * (CB_SIZE+1));

  pinMode(DEBUG_LED, OUTPUT);
  // pinMode(0, OUTPUT);
  // pinMode(2, OUTPUT);

  ledcSetup(leftChannel, pwmFrequency, pwmResolution);
  ledcSetup(rightChannel, pwmFrequency, pwmResolution);
  ledcAttachPin(PIN_LEFT_CHANNEL, leftChannel);
  ledcAttachPin(PIN_RIGHT_CHANNEL, rightChannel);
   
  //Setup serial port
  debugSerial.begin(DEBUG_SERIAL_BAUDRATE);
  debugSerial.println();

  debugSerial.println("\nAUDIFI - Audio over Wi-Fi");
  debugSerial.println("-------------------------\n");
  debugSerial.println("Initializing client..");

  //configure timer to run the callback function at ~44100Hz frequency
  // timer = timerBegin(0, 1814, true);  //44100Hz
  timer = timerBegin(0, 7256, true);  //11025Hz
  // timer = timerBegin(0, 65000, true);  //1000Hz
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1, true);
  timerAlarmEnable(timer);
  
  //Begin WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.mode(WIFI_STA);
  
  //Connecting to WiFi...
  debugSerial.print("Connecting to ");
  debugSerial.print(WIFI_SSID);
  
  //Loop continuously while WiFi is not connected
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    debugSerial.print(".");
  }
  
  // Connected to WiFi
//  Serial.println();
  debugSerial.println("\nConnected to AUDIFI server.");
  debugSerial.print("IP address: ");
  debugSerial.println(WiFi.localIP());
  debugSerial.print("RSSI: ");
  debugSerial.println(WiFi.RSSI());

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
 
  // Begin UDP port
  UDP.begin(UDP_PORT);
  debugSerial.print("Listening to UDP port ");
  debugSerial.println(UDP_PORT);

  // debugSerial.println("Receiving audio..");

  //create a new task to run on the Core 0. this will handle the interrupt job.
  xTaskCreatePinnedToCore (
    streamTask,   //Function to implement the task
    "streamTask", //Name of the task
    3000,             //Stack size in words
    NULL,              //Task input parameter
    5,                 //Priority of the task
    NULL,              //Task handle.
    0);                //Core where the task should run
}

//===================================================================//
  
void loop() {
  authenticateServer();

  if (isCbEmpty(&audioBuffer)) {
    audioBufferEmpty = true;
  }

  if (serverReady) {
    //if audio buffer is ever empty, fill it completely.
    if (audioBufferEmpty) {
      uint32_t entryTime = millis();

      //fill the buffer until it is full or until timeout.
      while ((!isCbFull(&audioBuffer)) && ((millis() - entryTime) < 30000)) {
        if (requestData() != -1) {
          if (tempAudioBufferLength > 0) {
            //push the received samples to the audio buffer.
            for (int i = REQUEST_HEADER_SIZE; i < (REQUEST_HEADER_SIZE + tempAudioBufferLength); i++) {
              cbPush(&audioBuffer, tempAudioBuffer[i]);
            }
          }
        }
        else {
          debugSerial.println("Data request failed");
          vTaskDelay(1000);
        }
      }
      
      //after the timeout, the CB doesn't have to be full always.
      //in such case, if the CB is non-empty, we can signal the streaming task
      //to start reading the CB.
      if (getCbOccupiedLength(&audioBuffer) > 0) {
        portENTER_CRITICAL(&bufferSignalMux);
          audioBufferEmpty = false;
        portEXIT_CRITICAL(&bufferSignalMux);
        debugSerial.println("Buffer has been filled");
      }
    }

    //if audio buffer is not empty, check how much space is left in the audio buffer.
    //if the space is larger than our data request size, we can make a new request and fill
    //the buffer.
    else {
      //check if there's enough space in CB to make a new request.
      if (getCbVacantLength(&audioBuffer) >= (REQUEST_SIZE-REQUEST_HEADER_SIZE)) {
        debugSerial.println("Requesting data..");
        
        if (requestData() != -1) {
          if (tempAudioBufferLength > 0) {
            //push the received samples to the audio buffer.
            for (int i = REQUEST_HEADER_SIZE; i < (REQUEST_HEADER_SIZE + tempAudioBufferLength); i++) {
              cbPush(&audioBuffer, tempAudioBuffer[i]);
            }
          }
        }
        else {
          debugSerial.println("Data request failed");
          vTaskDelay(1000);
        }
      }
    }
  }
}

//===================================================================//

int requestData (int length) {
  UDP.flush();

  udpRxPacketSize = 0;
  udpRxDataLength = 0;
  tempAudioBufferLength = 0;
  int packetFragmentCount = 0;  //no. of fragments we need to read
  int packetFragmentCounter = 0;  //no. of fragments we have read so far
  int byteCounter = 0;

  if (length <= UDP_MTU_SIZE) {
    packetFragmentCount = 1;
  }
  else {
    //find how many fragments of data we need to retrieve based on the MTU size.
    //calculate the mod of the length to determine if it is a multiple of MTU size.
    packetFragmentCount = ((length % UDP_MTU_SIZE) == 0) ? (length / UDP_MTU_SIZE) : (length / UDP_MTU_SIZE) + 1;
  }

  //send a request for data
  if (serverReady && (WiFi.status() == WL_CONNECTED)) {
    uint8_t buffer[] = "RD?";
    sendUDP(buffer, strlen((char*)buffer)); //send to client
  }
  
  uint32_t entryTime = millis();

  while ((packetFragmentCounter < packetFragmentCount) && ((millis() - entryTime) < 6000)) {
    udpRxPacketSize = UDP.parsePacket();

    if (udpRxPacketSize) {
      //read the data
      udpRxDataLength = UDP.read(udpRxDataBuffer, UDP_MTU_SIZE);

      //determine the no. of samples we need to read.
      //this has to be done only when reading the first fragment.
      if (packetFragmentCounter == 0) {
        tempAudioBufferLength = uint16_t(udpRxDataBuffer[0] << 8);  //high byte
        tempAudioBufferLength |= udpRxDataBuffer[1];  //low byte
      }

      //because we are receiving fragments each time.
      packetFragmentCounter++;

      //only if the sample length is valid.
      if ((tempAudioBufferLength > 0) && (tempAudioBufferLength <= (REQUEST_SIZE - REQUEST_HEADER_SIZE))) {
        int j = 0;
        //byteCounter will keep track of the index position in the temp buffer.
        for (int i = byteCounter; i < (byteCounter + udpRxDataLength); i++) {
          tempAudioBuffer[i] = udpRxDataBuffer[j];
          j++;
        }
        byteCounter += udpRxDataLength;

        if ((byteCounter >= length) || (byteCounter >= (tempAudioBufferLength + REQUEST_HEADER_SIZE))) {
          return byteCounter;
        }
      }
      else {
        debugSerial.print("Unexpected sample length: ");
        debugSerial.println(tempAudioBufferLength);
        return -1;
      }
    }
  }

  return byteCounter;
}

//===================================================================//

void udpCallResponse() {
  if (1) {
    udpRxDataLength = 0;
    udpRxDataBuffer[0] = '\0';
    udpRxPacketSize = UDP.parsePacket(); //listen to server
    if (udpRxPacketSize) {
      udpRxDataBuffer[0] = '\0';
      udpRxDataLength = UDP.read(udpRxDataBuffer, 255); //read the data to buffer
      if (udpRxDataLength > 0) {
        udpRxDataBuffer[udpRxDataLength] = '\0';
        
        debugSerial.print("Request received: ");
        debugSerial.print(udpRxDataBuffer);
        debugSerial.print(" ");
        debugSerial.println(strlen(udpRxDataBuffer));
      }
    }
  }

  if (1) {
    uint8_t buffer[] = "YES!";
    sendUDP(buffer, strlen((char*)buffer)); //send to client
    vTaskDelay(500);
  }
}

//===================================================================//
//checks if the circular buffer is full.

bool isCbFull(circ_buf_t* c) {
  if ((c->head + 1) == (c->tail)) {
    return true;
  }
  return false;
}

//===================================================================//

int getCbOccupiedLength(circ_buf_t* c) {
  return (c->head - c->tail);
}

//===================================================================//

int getCbVacantLength (circ_buf_t* c) {
  return (CB_SIZE - (c->head - c->tail));
}

//===================================================================//
//pushes a byte to the CB.

int cbPush (circ_buf_t* c, uint8_t data) {
  int next;

  next = c->head + 1;  // next is where head will point to after this write.
  
  if (next >= c->maxlen) {
    next = 0;
  }

  if (next == c->tail) {  // if the head + 1 == tail, circular buffer is full
    return -1;
  }

  c->buffer[c->head] = data;  // Load data and then move
  c->head = next;             // head to next data offset.
  return 0;  // return success to indicate successful push.
}

//===================================================================//
//checks if the circular buffer is empty.

bool isCbEmpty(circ_buf_t* c) {
  if (c->head == c->tail) {  // if the head == tail, we don't have any data
    return true;
  }
  return false;
}

//===================================================================//
//pops a byte from the CB.

int cbPop (circ_buf_t* c, uint8_t* data) {
  int next;

  if (c->head == c->tail) {  // if the head == tail, we don't have any data
    return -1;
  }

  next = c->tail + 1;  // next is where tail will point to after this read.
  if(next >= c->maxlen) {
    next = 0;
  }

  *data = c->buffer[c->tail];  // Read data and then move
  c->tail = next;              // tail to next offset.
  return 0;  // return success to indicate successful pop.
}

//===================================================================//

void wdtFeed() {
  //feed the WDT so that the MCU won't be reset if staying idle for long.
  //try disabling these lines to see the WDT reset in action.
  TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE;
  TIMERG0.wdt_feed=1;
  TIMERG0.wdt_wprotect=0;
}

//===================================================================//

uint32_t sendUDP(uint8_t* data, uint32_t length) {
  //send a UDP packet
  UDP.beginPacket(server_IP, UDP_PORT);
  uint32_t response = UDP.write((uint8_t*)data, length);
  UDP.endPacket();
  return response;
}

//===================================================================//

void authenticateServer() {
  if (!serverReady) {
    debugSerial.println("Authenticating server..");
    uint32_t entryTime = millis();
    
    while ((millis() - entryTime) <= 1000) {
      udpRxDataLength = 0;
      udpRxDataBuffer[0] = '\0';
      udpRxPacketSize = UDP.parsePacket(); //listen to server
    
      //if UDP data was received from server
      if (udpRxPacketSize) {
        udpRxDataLength = UDP.read(udpRxDataBuffer, 255); //read the data to buffer
        // UDP.flush();

        if (udpRxDataLength > 0) {
          udpRxDataBuffer[udpRxDataLength] = '\0';
          
          debugSerial.print("Request received: ");
          debugSerial.print(udpRxDataBuffer);
          debugSerial.print(" ");
          debugSerial.println(strlen(udpRxDataBuffer));
          
          if (strcmp(udpRxDataBuffer, "READY?") == 0) {
            uint8_t buffer[] = "YES!";
            sendUDP(buffer, strlen((char*)buffer));
            debugSerial.println("Server is ready");
            serverReady = true;
            return;
          }
        }
      }
      wdtFeed();
    }
    
    if (udpRxDataLength == 0) {
      debugSerial.println("No requests received");
    }
    else {
      debugSerial.print("Unexpected response received: ");
      debugSerial.print(udpRxDataBuffer);
      debugSerial.print(" ");
      debugSerial.println(strlen(udpRxDataBuffer));
    }
  }
  return;
}

//===================================================================//
