#include <windows.h>
#include <Timeapi.h>
#include <stdexcept>
#include <iostream>
#include "wit_c_sdk.h"
#include "REG.h"

// Change scheduler tick. Tell Windows to wake us up within a
// millisecond.
#pragma comment(lib, "winmm.lib") // for timeBeginPeriod/timeEndPeriod
class LowLatency
{
public:
   LowLatency() { timeBeginPeriod(1); }
   ~LowLatency() { timeEndPeriod(1); }
};

class SerialPort
{
   HANDLE hComm;
public:
   SerialPort(const char* name);
   ~SerialPort();
   void setBaudRate(unsigned newBaudRate);
   void setLowLatency();
   void write(const std::uint8_t* buffer, std::size_t buffer_size);
   std::size_t read(std::uint8_t* buffer, std::size_t buffer_size);
};

SerialPort::SerialPort(const char* name) :
    hComm(CreateFileA(name,
                      GENERIC_READ | GENERIC_WRITE, //Read/Write
                      0,                            // No Sharing
                      NULL,                         // No Security
                      OPEN_EXISTING,// Open existing port only
                      0,            // Non Overlapped I/O
                      NULL))        // Null for Comm Devices
{
   if (hComm == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Error in opening serial port");
}

SerialPort::~SerialPort()
{
   CloseHandle(hComm);
}

void SerialPort::setLowLatency()
{
   COMMTIMEOUTS commTimeouts;
   ::GetCommTimeouts(hComm, &commTimeouts);
   // this read timeout setup will return immediately with anything in
   // kernel buffer, and will wait up to 1 msec for new stuff to
   // arrive if it's empty
   commTimeouts.ReadIntervalTimeout = MAXDWORD;
   commTimeouts.ReadTotalTimeoutMultiplier = 0;
   commTimeouts.ReadTotalTimeoutConstant = 1;
   ::SetCommTimeouts(hComm, &commTimeouts);
}

int main(int argc, const char* argv[])
{
   if (argc < 2)
   {
      std::cerr << "usage: MPAWitMotionTestProgram port-name\n";
      return 1;
   }
   LowLatency lowLatency;
   SerialPort port(argv[1]);
   std::cout << "opening serial port successful\n";
   port.setLowLatency();
   WitInit(WIT_PROTOCOL_MODBUS, 0x50);
   //WitSerialWriteRegister(send); // register the routine to submit commands
   //WitRegisterCallBack(onReplySingleton); // register to handle response
   //WitDelayMsRegister(sleepMS); // register delay function
   //autobaud(); // requires singleton to use static callbacks!
   WitSetBandwidth(BANDWIDTH_256HZ);
   WitSetOutputRate(RRATE_200HZ);
   WitDeInit();
   return 0;
}

