#include <windows.h>
#include <Timeapi.h>
#include <conio.h> // _kbhit
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
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
   bool setBaudRate(unsigned newBaudRate);
   void setLowLatency();
   void write(const std::uint8_t* buffer, std::size_t buffer_size);
   std::size_t read(std::uint8_t* buffer, std::size_t buffer_size);
private:
   void throwCommError(const std::string& api);
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
      throwCommError("opening serial port");
}

SerialPort::~SerialPort()
{
   CloseHandle(hComm);
}

void SerialPort::setLowLatency()
{
   COMMTIMEOUTS commTimeouts;
   if (!::GetCommTimeouts(hComm, &commTimeouts))
      throwCommError("GetCommTimeouts");
   // this read timeout setup will return immediately with anything in
   // kernel buffer, and will wait up to 1 msec for new stuff to
   // arrive if it's empty
   commTimeouts.ReadIntervalTimeout = MAXDWORD;
   commTimeouts.ReadTotalTimeoutMultiplier = 0;
   commTimeouts.ReadTotalTimeoutConstant = 1;
   if (!::SetCommTimeouts(hComm, &commTimeouts))
      throwCommError("GetCommTimeouts");
}

// can return false if the device doesn't support the baud rate
bool SerialPort::setBaudRate(unsigned newBaudRate)
{
   DCB DCB_Struct_Parameter = {0};
   DCB_Struct_Parameter.DCBlength = sizeof(DCB_Struct_Parameter);
   BOOL status = GetCommState(hComm, &DCB_Struct_Parameter);
   if (!status)
      throwCommError("GetCommState");
   // 8N1
   DCB_Struct_Parameter.BaudRate = newBaudRate;
   DCB_Struct_Parameter.ByteSize = 8;
   DCB_Struct_Parameter.Parity   = NOPARITY;
   DCB_Struct_Parameter.StopBits = ONESTOPBIT;
   status = SetCommState(hComm, &DCB_Struct_Parameter);
   return 0 != status;
}

void SerialPort::write(const std::uint8_t* buffer, std::size_t buffer_size)
{
   DWORD bytesWritten;
   WriteFile(hComm, buffer, (DWORD)buffer_size, &bytesWritten, NULL);
}

std::size_t SerialPort::read(std::uint8_t* buffer, std::size_t buffer_size)
{
   DWORD bytesRead;
   ReadFile(hComm, buffer, (DWORD)buffer_size, &bytesRead, NULL);
   return bytesRead;
}

void SerialPort::throwCommError(const std::string& api)
{
   const DWORD win32_error_code = GetLastError();
   CHAR error_message[256];
   FormatMessageA( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL,
                   win32_error_code,
                   0,               
                   error_message,   
                   sizeof error_message,
                   NULL
                 );
   std::string message(api);
   message += " failed: ";
   message += error_message;
   throw std::runtime_error(message);
}

SerialPort* g_port = NULL; // for Wit callbacks
bool sampleReceived = false; // for autobaud, means we got a coherent reply with good checksum

void onWrite(std::uint8_t *p_ucData, std::uint32_t uiLen)
{
   g_port->write(p_ucData, uiLen);
}

void requestSample()
{
   WitReadReg(AX,3); // request data from device, accel, 3 axes
}

void sleepMS(std::uint16_t ucMs)
{
   std::this_thread::sleep_for(std::chrono::milliseconds(ucMs));
}

void autobaud()
{
   // attempt highest first, assuming we already set it on earlier run
   static const std::vector<unsigned> c_uiBaud { 230400, 4800, 9600, 19200, 38400, 57600, 115200 /*,460800*/};
   sampleReceived = false; // reset detector
   for (auto testBaud : c_uiBaud)
   {
      std::cout << "autobaud trying " << testBaud;
      const bool baudRateSupported = g_port->setBaudRate(testBaud);
      if (!baudRateSupported)
      {
         std::cout << " not supported\n";
         continue;
      }
      std::cout << '\n';
      int iRetry = 2;
      do
      {
         requestSample();
         sleepMS(100); // allow time for reply
         if (sampleReceived)
         {
            std::cout << "device found at baud " << testBaud << '\n';
            if (230400 != testBaud)
            {
               // try to change the device to fastest speed and see if it worked
               WitSetUartBaud(WIT_BAUD_230400);
               sleepMS(100);
               g_port->setBaudRate(230400);
               std::cout << "speed set to 230400\n";
            }
            return;
         }
         iRetry--;
      } while (iRetry);		
   }
   throw std::runtime_error("device not found by autobaud");
}

typedef std::vector<std::int16_t> Sample; // array of registers
typedef std::vector<Sample> Samples; // array of samples
static Samples samples;

void onReply(std::uint32_t uiReg, std::uint32_t uiRegNum)
{
   // Called when sReg is updated. uiReg is first register index,
   // uiRegNum is how many. Global sReg has the list of all registers.
   sampleReceived = true; // for autobaud logic
   Sample sample { sReg[AX], sReg[AY], sReg[AZ] };
   samples.emplace_back(sample);
   requestSample();
}

int main(int argc, const char* argv[])
{
   try
   {
      if (argc < 3)
         throw std::runtime_error("usage: MPAWitMotionTestProgram port-name log-file");
      LowLatency lowLatency;
      SerialPort port(argv[1]);
      // open the output file early to make sure we can write to it
      std::ofstream of(argv[2]);
      if (!of.is_open())
         throw std::runtime_error("failed to open output file for writing");
      g_port = &port; // for callbacks
      std::cout << "opening serial port successful\n";
      port.setLowLatency();
      WitInit(WIT_PROTOCOL_MODBUS, 0x50);
      WitSerialWriteRegister(onWrite); // register the routine to submit commands
      WitRegisterCallBack(onReply); // register to handle response
      WitDelayMsRegister(sleepMS); // register delay function
      autobaud(); // requires singleton to use static callbacks!
      WitSetBandwidth(BANDWIDTH_256HZ);
      WitSetOutputRate(RRATE_200HZ);
      while (!_kbhit())
      {
         std::uint8_t buffer[256];
         const std::size_t bytes_read = port.read(buffer, (DWORD)sizeof buffer);
         for (unsigned i = 0; i < bytes_read; i++)
            WitSerialDataIn(buffer[i]);
      }
      WitDeInit();
      g_port = NULL; // catch any late attempts to use this pointer
      // dump the samples
      for (Samples::const_iterator p = samples.begin(); p != samples.end(); ++p)
      {
         for (Sample::const_iterator q = p->begin(); q != p->end(); ++q)
         {
            of << *q;
            if (p->begin() != q)
               of << ',';
         }
         of << '\n';
      }
      Samples().swap(samples); // release memory by swapping with an empty vector
      return 0;
   }
   catch (const std::exception& e)
   {
      std::cerr << e.what() << '\n';
      return 1;
   }
}

