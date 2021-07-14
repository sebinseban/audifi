
//===================================================================//
//
//  -- AUDIFI Transmitter --
//
//  The transmitter end code for AUDIFI.
//
//  Version 0.3
//  Last modified : +05:30 16:54:34 PM 13-07-2021, Tuesday
//  
//===================================================================//

#include <WiFi.h>
// #include "esp_wifi.h"
#include <WiFiUdp.h>
#include "ptScheduler.h"
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"
#include "soc/rtc_wdt.h"

//===================================================================//
 
// Set AP credentials
#define AP_SSID "AUDIFI-SERVER"
#define AP_PASS "12345678"
#define UDP_PORT 4210

#define RXD2 16
#define TXD2 17

#define debugSerial Serial2
#define dataSerial Serial

#define DEBUG_SERIAL_BAUDRATE 500000
// #define DATA_SERIAL_BAUDRATE 2000000
#define DATA_SERIAL_BAUDRATE 500000

#define REQUEST_SIZE 11029  //size required for data
#define REQUEST_HEADER_SIZE 4 //size required for buffer header info

#define UDP_MTU_SIZE 1460

//===================================================================//
 
// UDP
WiFiUDP UDP;
IPAddress server_IP (192,168,4,1);
IPAddress client_IP (192,168,4,2);
IPAddress gateway (192,168,4,1);
IPAddress subnet (255,255,255,0);

//a task to print the total interrupt count.
ptScheduler printTask(1000000);
// ptScheduler monitorConnectionTask(200000);

volatile bool applicationReady = false;
volatile bool clientConnected = false;
volatile bool clientDisconnected = false;
volatile bool clientReady = false;
volatile bool dataRequestReceived = false;
volatile bool dataReady = false;
volatile bool serialDataIncoming = false;
volatile bool dataRequestAcknowledged = false;
volatile bool serialDataReadError = false;

//UDP receive buffer and parameters
int udpRxPacketSize = 0;  //the packet size with header data
int udpRxDataLength = 0;  //the length of samples in a packet (packet size - header)
char udpRxDataBuffer[UDP_MTU_SIZE] = {0};

//UDP transmit buffer and parameters
int udpTxPacketSize = 0;  //packet size with header data
int udpTxDataLength = 0;  //length of samples in a packet
uint8_t udpTxDataBuffer[REQUEST_SIZE] = {0};

//serial receive buffer for data serial
uint16_t serialRxDataLength = 0;
uint8_t serialRxDataBuffer[REQUEST_SIZE+50] = {0};

uint16_t sampleDataLength = 0;  //the length of samples read from serial buffer
uint32_t outgoingPacketCounter = 0; //the number of packets sent to the client

portMUX_TYPE criticalMux = portMUX_INITIALIZER_UNLOCKED;

//===================================================================//

void serialTask(void* pvParameters) {
  while(1) {  //infinite loop
    //if the windows application is not ready, try sending requests
    //and wait for a response.
    if (!applicationReady) {
      //the termination char is not included in the string.
      String serialRxString = dataSerial.readStringUntil('\n');
      if ((serialRxString == "READY?") || (serialRxString == "READY?\n")) {
        //send a response
        dataSerial.print("YES!\n");
        debugSerial.println("Application connected");
        applicationReady = true;
      }

      //if the response from application was different, wait for some time
      //before sending a new request.
      else {
        // debugSerial.print("Device is not ready. Response: ");
        // debugSerial.println(serialRxString);
        vTaskDelay(500);
      }
    }

    //if the application is ready, we ca request data to it.
    if (applicationReady) {
      //if a data request was made by the client.
      if (dataRequestReceived && (!serialDataIncoming)) {
        int requestRetryCount = 0;
        
        //every data request has to be acknoeledged by the application.
        //if a request was not acked, we can retry. but the retries has to
        //be limited to a certain number.
        while (dataRequestReceived && (!dataRequestAcknowledged)) {
          // debugSerial.println("\nRequesting data from application..");
          // dataSerial.flush();
          int excessDataCounter = 0;

          //this is to clear the buffer any remaining unread data from previous transfers.
          //we have to do this because, Arduino's flush() function no more does this.
          while (dataSerial.available() > 0) {
              dataSerial.read();
          }

          // debugSerial.print("Clearing excess bytes ");
          // debugSerial.println(excessDataCounter);

          //send a request. sending a newline at the end makes it easier
          //for the other end to read the data.
          dataSerial.print("RD?\n");

          //read the response from the application
          uint8_t serialRxString[6] = {0};
          dataSerial.readBytesUntil('\n', serialRxString, 6);

          //remove the NL
          serialRxString[4] = '\0';
          
          //if the ack was received, print the response.
          if (strcmp("ACK!", (char*)serialRxString) == 0) {
            dataRequestAcknowledged = true;
            // debugSerial.print("Response received: ");

            // //the response does not always need to be printable
            // for (int i=0; i<6; i++) {
            //   debugSerial.print((int)serialRxString[i]);
            //   if (i==5) debugSerial.println();
            //   else debugSerial.print(" ");
            // }

            // debugSerial.println("Application acknowledged data request");
            break;
          }
          else {
            // debugSerial.print("Response received: ");
              
            // for (int i=0; i<6; i++) {
            //   debugSerial.print((int)serialRxString[i]);
            //   if (i==5) debugSerial.println();
            //   else debugSerial.print(" ");
            // }

            requestRetryCount++;
            dataRequestAcknowledged = false;

            //if retries are exhausted
            if (requestRetryCount == 3) {
              // debugSerial.println("Failed to fetch data from application");
              requestRetryCount = 0;
              // debugSerial.println((char*)serialRxString);

              portENTER_CRITICAL(&criticalMux);
                dataRequestReceived = false;
                serialDataIncoming = false;
                dataRequestAcknowledged = false;
              portEXIT_CRITICAL(&criticalMux);
            }
            vTaskDelay(100);
          }
        }

        //when the application acks the data request.
        if (dataRequestAcknowledged) {
          if (dataSerial.available()) { //check if any data available
            uint8_t tempBuffer[2] = {0};
            //read the first two bytes that determines the incoming data length
            if (dataSerial.readBytes(tempBuffer, 2) == 2) {
              serialRxDataLength = 2; //because we just read two bytes
            } else {
              //this is the only place this var is reset.
              serialRxDataLength = 0;
            }
            
            portENTER_CRITICAL(&criticalMux);
              sampleDataLength = uint16_t(tempBuffer[0] << 8);  //high byte
              sampleDataLength |= tempBuffer[1];  //low byte
            portEXIT_CRITICAL(&criticalMux);
            
            if (sampleDataLength > 0) { //if length is non-zero
              if (sampleDataLength <= (REQUEST_SIZE - REQUEST_HEADER_SIZE)) { //if the length does't exceed
                serialDataIncoming = true;
                udpTxDataBuffer[0] = tempBuffer[0]; //save the length value
                udpTxDataBuffer[1] = tempBuffer[1];
                udpTxDataBuffer[2] = 0; //reserved
                udpTxDataBuffer[3] = 0; //reserved
                // debugSerial.print("Data incoming.. ");
                // debugSerial.println(sampleDataLength);
              }
              else {  //if the length is greater than what we are expecting
                serialDataIncoming = false;
                // debugSerial.print("Excess data available ");
                // debugSerial.println(sampleDataLength);
                // debugSerial.println("Data will be discarded.");
                serialDataIncoming = false;
                dataReady = false;
                dataRequestReceived = false;
                dataRequestAcknowledged = false;
                // vTaskDelay(500);
              }
            }
            else {  //if the length is zero
              serialDataIncoming = false;
              dataReady = false;
              dataRequestReceived = false;
              dataRequestAcknowledged = false;
              // debugSerial.println("No data available");
              // vTaskDelay(500);
            }
          }

          //after reading the first two bytes, we will know how many bytes to read.
          if (dataRequestReceived && serialDataIncoming) {
            //readBytes() is a timedout function unlike read().
            //the incoming data will be saved starting from index 0.
            //since we have already read two bytes, we only have to read the samples now.
            serialRxDataLength += dataSerial.readBytes(serialRxDataBuffer, sampleDataLength);

            //the data in the serial buffer has to be transferred to the UDP transmit buffer.
            //we cannot use memcpy() or others since we have an index offset.
            int j = 0;
            for (int i = REQUEST_HEADER_SIZE; i < (sampleDataLength + REQUEST_HEADER_SIZE); i++) {  //TODO
              udpTxDataBuffer[i] = serialRxDataBuffer[j];
              j++;
            }

            int excessBytesCounter = 0;

            //this counts the remaining or misread data bytes in the serial buffer.
            while (dataSerial.available() > 0) {
              if (dataSerial.read() != -1) {
                excessBytesCounter++;
              }
            }

            // //the max value of serialRxDataLength will be 2050 bytes in normal cases.
            // //because we only read 2 length bytes and a max of 2048 samples at a time.
            // debugSerial.print("Data has been read to buffer: ");
            // debugSerial.println(serialRxDataLength);
            // debugSerial.print("Excess bytes found: ");
            // debugSerial.println(excessBytesCounter);

            //check if we have read the same amount of bytes we should.
            if (serialRxDataLength != (sampleDataLength + 2)) {
              serialDataReadError = true; //if not, that's an error
            }
            else {
              serialDataReadError = false;
            }

            //at the end, reset everything so that we can fulfill another request.
            //also indicate the main thread that data is ready in the transmit buffer.
            portENTER_CRITICAL(&criticalMux);
              serialDataIncoming = false;
              dataReady = true; //this has to be reset at the main task, after reading the data
              dataRequestReceived = false;
              dataRequestAcknowledged = false;
            portEXIT_CRITICAL(&criticalMux);
          }
        }
      }
    }
    
    //if request for data was received but the application is not ready yet.
    //dataReady should not be reset here, because it pevents the main task from reading
    //any previously fetched data.
    if ((!applicationReady) && dataRequestReceived) {
      debugSerial.println("Data request received. But application is not ready.");
      portENTER_CRITICAL(&criticalMux);
        serialDataIncoming = false;
        // dataReady = false;
        dataRequestReceived = false;
        dataRequestAcknowledged = false;
      portEXIT_CRITICAL(&criticalMux);
    }
    wdtFeed();
  }
}

//===================================================================//

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info){
  if (clientConnected) {
    clientConnected = false;
    clientDisconnected = true;
    clientReady = false;
  }
}

//===================================================================//
 
void setup() {
  // Setup LED pin
  pinMode(LED_BUILTIN, OUTPUT);
   
  // Setup serial port
  dataSerial.begin(DATA_SERIAL_BAUDRATE);
  dataSerial.setRxBufferSize(REQUEST_SIZE+50);
  debugSerial.begin(DEBUG_SERIAL_BAUDRATE, SERIAL_8N1, RXD2, TXD2);
  debugSerial.println();
 
  // Begin Access Point
  debugSerial.println("\nAUDIFI - Audio over Wi-Fi");
  debugSerial.println("-------------------------\n");
  debugSerial.println("Starting AUDIFI server..");

  WiFi.softAPConfig(server_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASS);
  // debugSerial.println(WiFi.localIP());
  debugSerial.println("Local IP is 192.168.4.1");
 
  // Begin listening to UDP port
  UDP.begin(UDP_PORT);
  debugSerial.print("Opening UDP port ");
  debugSerial.println(UDP_PORT);

  debugSerial.println("Waiting for client..");

  // WiFi.onEvent(WiFiStationConnected, SYSTEM_EVENT_AP_STACONNECTED);
  WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_AP_STADISCONNECTED);

  //create a new task to run on the Core 0. this will handle the interrupt job.
  xTaskCreatePinnedToCore (
    serialTask,   //Function to implement the task
    "serialTask", //Name of the task
    2000,             //Stack size in words
    NULL,              //Task input parameter
    5,                 //Priority of the task
    NULL,              //Task handle.
    0);                //Core where the task should run
}

//===================================================================//
 
void loop() {
  waitForClient();
  authenticateClient();
  // udpCallResponse();  //this will not work after the client is reset and reconnected
  // commandMode();

  if (clientConnected && clientReady) { //if ready to accept requests from client
    udpRxDataLength = 0;
    udpRxDataBuffer[0] = '\0';
    udpRxPacketSize = UDP.parsePacket(); //listen to server

    if (udpRxPacketSize) {
      udpRxDataLength = UDP.read(udpRxDataBuffer, UDP_MTU_SIZE); //read the data to buffer
      
      if (udpRxDataLength > 0) {  //if data is valid
        udpRxDataBuffer[udpRxDataLength] = '\0';
        
        // //print the request received
        // debugSerial.print("Request received: ");
        // debugSerial.print(udpRxDataBuffer);
        // debugSerial.print(" ");
        // debugSerial.println(strlen(udpRxDataBuffer));
        
        //check if the request is to send data
        if (strcmp(udpRxDataBuffer, "RD?") == 0) {
          dataRequestReceived = true;
          // debugSerial.println("Request received to send data");
        }
      }
    }
  }

  if (dataReady) {
    // //print the lenght of bytes received from application, after a request was made.
    // debugSerial.print("Data received. Length: ");
    // debugSerial.println(serialRxDataLength);

    outgoingPacketCounter++;
    debugSerial.println(outgoingPacketCounter);
    
    sendUDP(udpTxDataBuffer, (sampleDataLength + REQUEST_HEADER_SIZE)); //send the data to receiver

    portENTER_CRITICAL(&criticalMux);
      dataReady = false;
    portEXIT_CRITICAL(&criticalMux);
  }

  wdtFeed();
}

//===================================================================//

uint32_t sendUDP(uint8_t* data, uint32_t length) {
  //send a UDP packet
  UDP.beginPacket(client_IP, UDP_PORT);
  uint32_t response = UDP.write((uint8_t*)data, length);
  UDP.endPacket();
  return response;
}

//===================================================================//

void udpCallResponse() {
  if (clientConnected) {
    uint8_t buffer[] = "READY?";
    sendUDP(buffer, strlen((char*)buffer)); //send to client
    vTaskDelay(500);
  }

  if (clientConnected) {
    udpRxDataLength = 0;
    udpRxDataBuffer[0] = '\0';
    udpRxPacketSize = UDP.parsePacket(); //listen to server

    if (udpRxPacketSize) {
      udpRxDataLength = UDP.read(udpRxDataBuffer, UDP_MTU_SIZE); //read the data to buffer
      
      if (udpRxDataLength > 0) {
        udpRxDataBuffer[udpRxDataLength] = '\0';
        
        debugSerial.print("Response received: ");
        debugSerial.print(udpRxDataBuffer);
        debugSerial.print(" ");
        debugSerial.println(strlen(udpRxDataBuffer));
      }
    }
  }
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

void waitForClient() {
  //wait until a client is connected
  if (clientDisconnected) {
    debugSerial.println("Client disconnected");
    clientDisconnected = false;
    clientConnected = false;
    clientReady = false;
    UDP.stop();
  }

  if (!clientConnected) {
    uint8_t stationCount = WiFi.softAPgetStationNum();  //get the no. of stations connected
    
    if (stationCount >= 1) {
      debugSerial.println("Client connected at 192.168.4.2");
      clientConnected = true;
      clientDisconnected = false;
      clientReady = false;
      UDP.begin(UDP_PORT);
      return;
    }
  }
  return;
}

//===================================================================//

void authenticateClient() {
  if (clientConnected && (!clientReady)) {
    debugSerial.println("Authenticating client..");
    uint8_t buffer[] = "READY?";

    byte bytesCount = sendUDP(buffer, strlen((char*)buffer)); //send to client

    if (bytesCount != strlen((char*)buffer)) {
      debugSerial.println("UDP write failed");
      return;
    }
    // vTaskDelay(500);
    
    uint32_t entryTime = millis();
    
    while ((millis() - entryTime) <= 1000) {
      // byte bytesCount = sendUDP(buffer, strlen((char*)buffer)); //send to client
      // vTaskDelay(100);

      udpRxDataBuffer[0] = '\0';
      udpRxDataLength = 0;
      udpRxPacketSize = UDP.parsePacket();
      
      if (udpRxPacketSize) {
        udpRxDataLength = UDP.read(udpRxDataBuffer, UDP_MTU_SIZE);
        // UDP.flush();

        if (udpRxDataLength > 0) {
          udpRxDataBuffer[udpRxDataLength] = '\0';

          debugSerial.print("Response received: ");
          debugSerial.print(udpRxDataBuffer);
          debugSerial.print(" ");
          debugSerial.println(strlen(udpRxDataBuffer));

          if (strcmp(udpRxDataBuffer, "YES!") == 0) {
            debugSerial.println("Client is authenticated");
            clientReady = true;
            return;
          }
        }
      }
      wdtFeed();
    }
    
    if (udpRxDataLength == 0) {
      debugSerial.println("No response received");
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

void commandMode() {
  uint8_t response = 0;
  String inputString = "";
  String commandString = "";
  String firstParam = "";
  String secondParam = "";
  String thirdParam = "";

  //send commands and parameters for each operation
  //items are separated by single whitespace
  //you can send up to 3 parameters
  if(debugSerial.available()) {  //monitor the serial interface
    inputString = debugSerial.readString();  //read the contents of serial buffer as string
    // Serial.print(F("Command : "));
    // Serial.println(inputString);
    // Serial.println();

    //-------------------------------------------------------------------------//

    uint8_t posCount = 0;
    int indexOfSpace = 0;

    while(inputString.indexOf(" ") != -1) { //loop until all whitespace chars are found
      indexOfSpace = inputString.indexOf(" ");  //get the position of first whitespace
      if(indexOfSpace != -1) {  //if a whitespace is found
        if(posCount == 0) //the first one will be command string
          commandString = inputString.substring(0, indexOfSpace); //end char is exclusive
        if(posCount == 1) //second will be second param
          firstParam = inputString.substring(0, indexOfSpace);
        if(posCount == 2) //and so on
          secondParam = inputString.substring(0, indexOfSpace);
        else if(posCount == 3)
          thirdParam = inputString.substring(0, indexOfSpace);
        inputString = inputString.substring(indexOfSpace+1);  //trim the input string
        posCount++;
      }
    }

    //saves the last part of the string is no more whitespace is found
    if(posCount == 0) //if there's just the command
      commandString = inputString;
    if(posCount == 1)
      firstParam = inputString;
    if(posCount == 2)
      secondParam = inputString;
    if(posCount == 3)
      thirdParam = inputString;

    if (commandString == "rq") {
      dataRequestReceived = true;
    }
  }
}

//===================================================================//
