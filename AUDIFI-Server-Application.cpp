
//==============================================================================//
//
//  AUDIFI Server Application
//  Version : v0.4
//  Last modified : +05:30 19:52:35 PM 13-07-2021, Tuesday
//
//==============================================================================//
/*

+----------------------+           +----------------+                  +-----------------+
|                      |           |                |                  |                 |
|                      |    USB    |                |    Wi-Fi (UDP)   |                 |
|  Server Application  |-----------|   Transmitter  |- - - - - - - - - |     Receiver    |
|                      |           |                |                  |                 |
|                      |           |                |                  |                 |
+----------------------+           +----------------+                  +-----------------+


*/
//==============================================================================//

//includes
#include <windows.h>
#include <stdio.h>
#include <string>
#include <iostream>

//a request originates at the client/receiver.
//the request size depends on the buffer sizes.
//the request size may be larger or smaller than the UDP MTU.
//if it is larger, multiple UDP packets are needed to fulfill
//a single request. every request will include 4 bytes of header
//information at the start of the sequence. first two bytes indicates
//the number of audio samples cotained in the request, so that the
//receiver can stop at the end of a song. the next two bytes in the header
//are reserved for future use.
#define REQUEST_SIZE 11029  //number of bytes per request
#define REQUEST_HEADER_SIZE 4 //bytes containing header information
#define REQUEST_DATA_SIZE REQUEST_SIZE-REQUEST_HEADER_SIZE //size of actual data in a request

#define TX_DATA_BUFFER_MAX_LENGTH 1024    //max size of serial transmit buffer
#define RX_DATA_BUFFER_MAX_LENGTH 1024    //max size of serial receive buffer
#define SERIAL_READ_TIMEOUT 2000          //time to wait for serial data
#define MAX_FILE_COUNT  10                //max number of audio files in a playlist
#define MAX_LINE_COUNT  MAX_FILE_COUNT    //max lines to scan on a playlist 
#define MAX_FILE_PATH_LENGTH  256         //max lnegth of path to an audio file
// #define SERIAL_BAUDRATE 2000000
#define SERIAL_BAUDRATE 500000            //speed at which data is sent to transmitter

//==============================================================================//
//Globals

DWORD serialBytesSent = 0;  //number of bytes sent via serial
DWORD serialBytesRead = 0;  //number of bytes read from serial

HANDLE hSerial;
DCB dcbSerialParams = {0};
COMMTIMEOUTS timeouts = {0};
COMSTAT hSerialStatus = {0};
LPDWORD hSerialError = 0;

std::string inputString = "";

bool delimFound = false;  //true when the delimiter character is found
int rxDataBufferIndex = 0;  //index var for receive buffer
int txDataBufferIndex = 0;  //index var for transmit buffer
char txDataBuffer[TX_DATA_BUFFER_MAX_LENGTH] = {0}; //transmit buffer
char rxDataBuffer[RX_DATA_BUFFER_MAX_LENGTH] = {0}; //receive buffer
uint16_t txDataLength = 0;  //length of data in tx buffer
uint16_t rxDataLength = 0;  //length of data in rx buffer
uint16_t sampleCounter = 0; //audio sample byte counter

FILE *playlistFptr; //the playlist file containing the list of audio files
FILE *audioFptr;

char filePathList[MAX_LINE_COUNT][MAX_FILE_PATH_LENGTH] = {0};
int fileCount = 0;

int comPortNumber = -1;

bool serialEstablished = false;
bool serialDisconnected = false;
bool serialReadTimedout = false;
bool serverReady = false;
bool dataRequestReceived = false;
bool dataOutgoing = false;

char playlistFileName[] = "Playlist-001.txt";

int lineBreaks[MAX_LINE_COUNT] = {0};
int byteBuffer = -1;
int seekPosition = 0;
int lineCount = 0;
int lineLength = 0;
int lineLengthList[MAX_LINE_COUNT] = {0};

//==============================================================================//
//Function declarations

void loop();
bool openComPort();
bool promptComPort();
int readSerial(uint32_t length, bool waitForDelim=false);
bool writeSerial(uint32_t length, bool appendDelim=true);
bool writeSerial(uint8_t* buffer, uint32_t length);
int streamAudio();
int readPlaylist();
bool checkDevice();

//==============================================================================//

int main() {
  printf("\nAUDIFI - Audio over Wi-Fi\n");
  printf("-------------------------\n");
  printf("\nPlease connect the AUDIFI server device.\n");

  while(!serialEstablished) {
    if (!promptComPort()) {
      printf("\nDevice discovery failed.\n");
      printf("Try reconnecting the device and enter the correct COM port.\n");
    }
  }

  checkDevice();
  readPlaylist();
  streamAudio();
  return 0;
}

//==============================================================================//

void loop() {

  return;
}

//==============================================================================//

int streamAudio() {
  if (serialEstablished) {
    for (int i=0; i < lineCount; i++) {
      //open a file from the path found in the playlist file.
      //once a file is fully streamed, we can open a new file.
      //the file has to be opened in binary mode, otherwise
      //fgetc will return bad values.
      audioFptr = fopen(filePathList[i], "rb");

      if (audioFptr == NULL) {
        printf("Failed to load audio file at %d\n", i);
      }
      else {  //if audio file is valid
        //reset the pointer to end so that we can get the size of the file
        fseek(audioFptr, 0, SEEK_END);
        int memorySize = ftell(audioFptr);

        if (memorySize > 44) {
          //allocate memory for the file
          uint8_t* memPointer = (uint8_t*) malloc(memorySize * sizeof(uint8_t));

          if (memPointer != NULL) { //if allocation was successful
            printf("Memory allocation successful for %d. Memory size: %d\n", i, memorySize);
            printf("Loading audio file to memory..\n");
            fseek(audioFptr, 0, SEEK_SET);  //reset to start so that we can read the file
            
            //load the audio file to allocated memory
            for (int i=0; i < memorySize; i++) {
              memPointer[i] = uint8_t(fgetc(audioFptr));
            }

            printf("Streaming audio..\n");
            
            //create a two byte buffer so that we can send L and R samples
            uint8_t pairBuffer[2] = {0};

            printf("Waiting for server request..\n");
            
            //loop until all data is sent
            for (int i=44; i < memorySize; i++) {
              //wait for a data request from server device
              while (!dataRequestReceived) {
                // printf("Waiting for server request..\n");
                //Arduino's println sends \r\n
                if (readSerial(4, true) == 4) { //read the incoming request from server
                  rxDataBuffer[3] = 0;  //remove the NL
                  if (strcmp("RD?", rxDataBuffer) == 0) {
                    // printf("Data request received\n");
                    uint8_t tempBuffer[] = "ACK!\n";
                    writeSerial(tempBuffer, strlen((char*)tempBuffer));
                    dataRequestReceived = true; //move to next step
                    dataOutgoing = false;
                    PurgeComm(hSerial, PURGE_RXCLEAR);
                  }
                  else {
                    if (strlen((char*)rxDataBuffer) > 0) {
                      printf("Data request incomplete: %s, %d\n", rxDataBuffer, strlen((char*)rxDataBuffer));
                    }
                    Sleep(500);
                  }
                }
                else {
                  if (strlen((char*)rxDataBuffer) > 0) {
                    printf("Data request incomplete: %s, %d\n", rxDataBuffer, strlen((char*)rxDataBuffer));
                  }
                  Sleep(500);
                }
              }

              //if a request was received from server
              if (dataRequestReceived) {
                //for every single request we need to calculate remaining no. of samples only once
                if (!dataOutgoing) {
                  if ((memorySize-i) < REQUEST_DATA_SIZE) {  //check if there's data left to fill the buffer entirely
                    txDataLength = memorySize - i;  //calculate no. of remaining samples
                  }
                  else {
                    txDataLength = REQUEST_DATA_SIZE;
                  }
                  uint8_t tempBuffer[2];
                  tempBuffer[0] = uint8_t(txDataLength >> 8); //high byte
                  tempBuffer[1] = uint8_t(txDataLength & 0x00FF); //high byte
                  writeSerial(tempBuffer, 2); //send the the data length bytes
                  dataOutgoing = true;  //actual samples can now be sent
                  sampleCounter = 0;
                  // printf("Request received. Sending %u samples..\n", txDataLength);
                }

                //send the requested amount of samples
                if (sampleCounter <= txDataLength) {
                  if ((i % 2) == 0) { //left channel samples
                    pairBuffer[0] = memPointer[i];
                    // pairBuffer[1] = 'L';
                    writeSerial(pairBuffer, 1);
                    // printf("%c %u %d\n", pairBuffer[0], pairBuffer[1], i);
                    // Sleep(500);
                    sampleCounter++;  //increment until the requested amount of samples
                  } else {
                    // pairBuffer[0] = memPointer[i];
                    // // pairBuffer[1] = 'R';
                    // writeSerial(pairBuffer, 1);
                    // // printf("%c %u %d\n", pairBuffer[0], pairBuffer[1], i);
                    // // Sleep(500);
                  }
                  // sampleCounter++;  //increment until the requested amount of samples
                }
                
                //once the requested amount of samples are sent
                if (sampleCounter == txDataLength) {
                  // printf("Samples sent: %d\n", sampleCounter);
                  sampleCounter = 0;  //reset
                  // txDataLength = 0;
                  dataOutgoing = false;
                  dataRequestReceived = false;  //can wait for the next request now
                  PurgeComm(hSerial, PURGE_RXCLEAR);
                }
              }
            }
            free(memPointer); //once all data is read
          } else {
            printf("Memory allocation failed for track %d", i);
            exit(1);
          }
        } else {
          printf("Audio file %d is not valid.\n", i);
        }
        fclose(audioFptr);
      }
    }
  }
  return 1;
}

//==============================================================================//
//Opens a text file containing absolute paths of audio files.
//Each line holds a single path. This will scan all lines and save the paths to
//an array so that we can open the audio files later.

int readPlaylist() {
  //open the file in read mode
  playlistFptr = fopen(playlistFileName, "r");

  if (playlistFptr != NULL) { //if playlist file is valid
    fseek(playlistFptr, 0, SEEK_SET); //reset the file read pointer to the start
    //this does something similar to what fgets does.
    //but you have better control over reading the lines.
    //a custom delimiter can be used and the delim position
    //is saved for easy file seeking, though this is not necessary
    //for this application.
    do {
      byteBuffer = fgetc(playlistFptr);  //get a single char and advance postion

      if (byteBuffer != EOF) { //if EOF has not reached
        // printf("%c", c);
        seekPosition++; //actually this could be replaced with ftell()
        lineLength++; //the length of line currently being scanned
        if (byteBuffer == '\n') {  //if an NL is found
          if (lineCount < MAX_LINE_COUNT) { //if the line length reaches the set max
            lineBreaks[lineCount] = seekPosition-1; //save the position of the NL
            lineLengthList[lineCount] = lineLength; //inlcudes delim char
          }
          lineCount++;  //number of lines found
          lineLength = 0; //reset length to scan the next line
        }
      }
      else {  //find a line with no NL but ends with EOF
        if (lineLength != 0) {
          lineCount++;  //the last line with non-zero chars and no NL is still a line
          // lineLength = 0;
        }
      }
    } while (byteBuffer != EOF);  //scan until the EOF

    //------------------------------------------------------------------------------//

    //print the parameters
    printf("\nLine count: %d\n", lineCount);
    printf("Last line length: %d\n", lineLength);
    printf("Last file seek position: %d\n", seekPosition);
    printf("Line break positions: ");

    //print the line break positions
    for (int i=0; i < lineCount; i++) {
      printf("%d ", lineBreaks[i]);
    }
    printf("\n");

    //print line lengths
    printf("Line lengths: ");

    for (int i=0; i < lineCount; i++) {
      printf("%d ", lineLengthList[i]);
    }
    printf("\n");

    //------------------------------------------------------------------------------//

    //scan the playlist file again for lines using linebreak and line count info
    fseek(playlistFptr, 0, SEEK_SET);
    
    for (int i=0; i < lineCount; i++) {
      for(int j=0; j < lineBreaks[i]; j++) {
        filePathList[i][j] = fgetc(playlistFptr);
        // filePathList[i][j] = (filePathList[i][j] == '\n') ? 0 : filePathList[i][j];
      }
    }

    // //print the lines
    // printf("Lines:\n");

    // for (int i=0; i < lineCount; i++) {
    //   for(int j=0; j < lineBreaks[i]; j++) {
    //     printf("%c", filePathList[i][j]);
    //   }
    // }
    // printf("\n");

    if (lineCount > 0) {
      printf("%d audio file(s) found in the playlist.\n", lineCount);
    } else {
      printf("No audio files were found.\n");
      return 0;
    }
    fclose(playlistFptr);
  } else {
    printf("Could not open playlist file %s\n", playlistFileName);
    return 0;
  }
  return lineCount;
}

//==============================================================================//
//This sends a "READY" query to the connected device. The device has to respond
//with "YES". Only then the interface is validated.

bool checkDevice() {
  if (serialEstablished) {  //only if serial port was established
    printf("Checking if device is ready..\n");
    // PurgeComm(hSerial, PURGE_RXCLEAR);
    // printf("Purged buffers\n");
    
    do {  //repeat until confirmation is received
      PurgeComm(hSerial, PURGE_RXCLEAR);  //purge RX buffer so that we can read clean
      strcpy(txDataBuffer, "READY?");  //copy the command to transmit buffer
      writeSerial(6); //NL will be included which makes the total char count 6
      Sleep(500);  //works wihout delay

      if (readSerial(4) == 4) { //read three chars and the return indicates the no. of chars read before timeout
        rxDataBuffer[4] = 0;  //null terminate the buffer

        if (strcmp("YES!", rxDataBuffer) == 0) { //compare the strings
          printf("Device is ready : %s\n", rxDataBuffer);
          Sleep(500);
          serverReady = true; //device is now ready
          PurgeComm(hSerial, PURGE_RXCLEAR);
          return true;
        }
        else {
          printf("Device is not ready.\n");
          serverReady = false;
          Sleep(1000);
        }
      }
      else {  //when no reply is received within the timeout period
        printf("Serial timed out. Device is not ready.\n");
        serverReady = false;
      }
    } while (!serverReady);
  }
  return false;
}

//==============================================================================//
//This asks the user to enter the COM port number of the connected server device.
//If the COM port can not be found or it is busy, this will fail.

bool promptComPort() {
  printf("Enter COM port number: ");
  scanf("%d", &comPortNumber);
  printf("Connecting to COM port %d..\n", comPortNumber);

  if (comPortNumber >= 0) {
    return openComPort(); //try opening the COM port
  }
  return false;
}

//==============================================================================//
//This actually opens the COM port.

bool openComPort () {
  char comPortNumberString[10] = {0};
  itoa(comPortNumber, comPortNumberString, 10); //convert the number to c-string
  char comPortString[20] = "\\\\.\\COM";  //format the string
  strcat(comPortString, comPortNumberString); //add the COM port number at the end

  // printf("Com port string is %s\n", comPortString);

  hSerial = CreateFileA (comPortString,                //port name
                      GENERIC_READ | GENERIC_WRITE, //Read/Write
                      0,                            // No Sharing
                      NULL,                         // No Security
                      OPEN_EXISTING,// Open existing port only
                      0,            // Non Overlapped I/O
                      NULL);        // Null for Comm Devices

  if (hSerial == INVALID_HANDLE_VALUE) {
    printf("Error opening serial port.\n");
  }
  else {
    printf("Opening serial port successful.\n");
    // CloseHandle(hSerial);
  }

  //------------------------------------------------------------------------------//
  //Serial port configuration

  dcbSerialParams.DCBlength = sizeof(dcbSerialParams);

  if (GetCommState(hSerial, &dcbSerialParams)) {  //try to read the serial port parameters
    printf("Getting COM status successful.\n");
  }
  else {
    printf("Getting COM status failed.\n");
    return false;
  }

  //add parameters
  dcbSerialParams.BaudRate = SERIAL_BAUDRATE;
  dcbSerialParams.ByteSize = 8;
  dcbSerialParams.StopBits = ONESTOPBIT;
  dcbSerialParams.Parity = NOPARITY;

  if (SetCommState(hSerial, &dcbSerialParams)) {  //set parameters
    printf("Setting COM parameters successful.\n");
  }
  else {
    printf("Setting COM parameters failed.\n");
    return false;
  }

  //------------------------------------------------------------------------------//
  //Set serial port timeouts

  timeouts.ReadIntervalTimeout = MAXDWORD;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  timeouts.ReadTotalTimeoutConstant =  0;
  timeouts.WriteTotalTimeoutConstant = 0;
  timeouts.WriteTotalTimeoutMultiplier = 0;

  if (SetCommTimeouts(hSerial, &timeouts)) {  //set parameters
    printf("Setting timeout parameters successful.\n");
    serialEstablished = true;
  }
  else {
    printf("Setting timeout parameters failed.\n");
    serialEstablished = false;
  }

  return true;
}

//==============================================================================//
//Writes a series of bytes to the serial port.
//The data first have to be saved to the txDataBuffer array. It has a max length of
//1024 bytes. First parameter is the number of bytes you want to write.
//You have the option to apped a delimiter character with your data. The second
//parameter tells this. An NL char is added at the end of the data by default.
//Note that the delmiter will repalce a character on the data buffer.

bool writeSerial(uint32_t length, bool appendDelim) {
  if (serialEstablished) {
    if (appendDelim) {
      if (length < TX_DATA_BUFFER_MAX_LENGTH) { //only if there's space
        txDataBuffer[length] = '\n';  //add the delimiter
        length++;
      }
    }
    if (WriteFile(hSerial, txDataBuffer, length, &serialBytesSent, NULL)) {
      return true;
    }
  }
  return false;
}

//==============================================================================//

bool writeSerial(uint8_t* buffer, uint32_t length) {
  if (serialEstablished) {
    if (WriteFile(hSerial, buffer, length, &serialBytesSent, NULL)) {
      return true;
    }
  }
  return false;
}

//==============================================================================//
//This reads a series of bytes from the serial port. First parameter is the no. of
//bytes you want to read. The max no. of bytes you can read is 1024 bytes.
//Second parameter is to whether wait for a delimiter character. If a delimiter is
//specified, the read finishes before reaching the specified no. of bytes. Means, 
//the length is ignored if you specify to wait for a delim.
//The delim is included in the returned data length and the data buffer.

int readSerial(uint32_t length, bool waitForDelim) {
  if (serialEstablished) {
    DWORD entryTime = GetTickCount();
    char oneByteBuffer[1] = {0};
    serialBytesRead = 0;
    rxDataBufferIndex = 0;
    
    //Read from serial port for the specified duration
    while ((GetTickCount() - entryTime) < SERIAL_READ_TIMEOUT) {
      // printf("Reading bytes..\n");
      if (ReadFile(hSerial, oneByteBuffer, 1, &serialBytesRead, NULL)) { //read one byte
        rxDataBuffer[rxDataBufferIndex] = oneByteBuffer[0]; //copy the read byte to main buffer
        // printf("Received 1 byte %u\n", rxDataBuffer[rxDataBufferIndex]);
        
        if (rxDataBufferIndex < RX_DATA_BUFFER_MAX_LENGTH) {
          rxDataBufferIndex++;  //advance the buffer position once
        }

        //if the specified number of bytes have been read or the max limit has reached
        if ((rxDataBufferIndex == RX_DATA_BUFFER_MAX_LENGTH) || ((!waitForDelim) && (rxDataBufferIndex >= length))) {
          serialReadTimedout = false;
          delimFound = (oneByteBuffer[0] == '\n') ? true : false;
          return rxDataBufferIndex;
        }
        
        //if the delmiter was found
        if ((waitForDelim) && (oneByteBuffer[0] == '\n')) {
          delimFound = true;
          serialReadTimedout = false;
          return rxDataBufferIndex;
        }
      }
      else {
        // printf("Reading serial failed\n");
        // serialEstablished = false;
        // serialDisconnected = true;
        // CloseHandle(hSerial);
        return -1;
      }
    }

    serialReadTimedout = true;
    delimFound = (oneByteBuffer[0] == '\n') ? true : false;
    return rxDataBufferIndex;
  }
  printf("Serial connection is not ready\n");
  return -1;
}

//==============================================================================//
