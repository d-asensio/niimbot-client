#include <vector>
#include <queue>

#include <Arduino.h>

#include <BLEDevice.h>
#include <BLEServer.h>

#define PRINTER_DEVICE_NAME "B1-G121131120"

static BLEUUID niimbotB1ServiceUUID("E7810A71-73AE-499D-8C15-FAA9AEF0C3F2");
static BLEUUID printerCommunicationCharacteristicUUID("BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F");

static BLEAddress *printerDeviceAddress = nullptr;
static BLERemoteCharacteristic *printerCommunicationCharacteristic = nullptr;

static boolean attemptConnectionToPrinter = false;
static boolean connectedToPrinter = false;
static boolean printing = false;

typedef std::vector<uint8_t> PrinterCommand;

std::queue<PrinterCommand> printerCommands;

namespace PrinterCommands
{
  const uint8_t CALIBRATE_LABEL_GAP = 0x8E;
  const uint8_t HEARTBEAT = 0xDC;
  const uint8_t GET_PRINT_STATUS = 0xA3;
  const uint8_t GET_LABEL_RFID = 0x1A;
  const uint8_t SET_LABEL_TYPE = 0x23;
  const uint8_t SET_PRINT_DENSITY = 0x21;
  const uint8_t START_LABEL_PRINT_DATA_EXCHANGE = 0x01;
  const uint8_t SET_PRINT_DIMENSIONS = 0x13;
  const uint8_t END_LABEL_PRINT_DATA_EXCHANGE = 0xE3;
  const uint8_t END_PRINT = 0xF3;
  const uint8_t PRINT_LINE = 0x85;
  const uint8_t PRINT_WHITESPACE = 0x84;
}

PrinterCommand calculateXor(const PrinterCommand &command)
{
  PrinterCommand result;

  if (command.empty())
  {
    return result; // return an empty result in case of empty input
  }

  uint8_t xor_value = command[0]; // Start with the first element
  for (size_t i = 1; i < command.size(); ++i)
  {
    xor_value ^= command[i]; // Apply the XOR operation with each subsequent element
  }

  result.push_back(xor_value); // push final XOR value to the result
  return result;
}

PrinterCommand createPacket(const PrinterCommand &bodySeq)
{
  PrinterCommand startSeq = {0x55, 0x55};
  PrinterCommand checksumSeq = calculateXor(bodySeq);
  PrinterCommand endSeq = {0xAA, 0xAA};

  PrinterCommand command = {};

  command.insert(command.end(), startSeq.begin(), startSeq.end());
  command.insert(command.end(), bodySeq.begin(), bodySeq.end());
  command.insert(command.end(), checksumSeq.begin(), checksumSeq.end());
  command.insert(command.end(), endSeq.begin(), endSeq.end());

  return command;
}

PrinterCommand createCommand(uint8_t commandCode, const PrinterCommand &bodySeq)
{
  PrinterCommand commandCodeSeq = {commandCode};
  PrinterCommand dataSizeSeq = {static_cast<uint8_t>(bodySeq.size())};

  PrinterCommand command = {};

  command.insert(command.end(), commandCodeSeq.begin(), commandCodeSeq.end());
  command.insert(command.end(), dataSizeSeq.begin(), dataSizeSeq.end());
  command.insert(command.end(), bodySeq.begin(), bodySeq.end());

  return createPacket(command);
}

void printHexData(uint8_t *data, size_t length)
{
  for (int i = 0; i < length; i++)
  {
    uint8_t chunk = *(data + i);

    Serial.print(" ");

    if (chunk < 0x0f)
    {
      Serial.print(0);
    }

    Serial.print(chunk, HEX);
  }
}

void sendCommand(PrinterCommand &command)
{
  printerCommunicationCharacteristic->writeValue(command.data(), command.size(), true);
}

void sendCalibrateLabelGapSignal()
{
  PrinterCommand command = createCommand(PrinterCommands::CALIBRATE_LABEL_GAP, {0x01});
  sendCommand(command);
}

void sendHeartbeatSignal()
{
  PrinterCommand command = createCommand(PrinterCommands::HEARTBEAT, {0x04});

  sendCommand(command);
}

void sendGetPrintStatus()
{
  PrinterCommand command = createCommand(PrinterCommands::GET_PRINT_STATUS, {0x01});

  sendCommand(command);
}

void sendGetRFID()
{
  PrinterCommand command = createCommand(PrinterCommands::GET_LABEL_RFID, {0x01});

  sendCommand(command);
}

void sendSetLabelType()
{
  PrinterCommand command = createCommand(PrinterCommands::SET_LABEL_TYPE, {0x01});

  sendCommand(command);
}

void sendSetDensity(uint8_t density)
{
  PrinterCommand command = createCommand(PrinterCommands::SET_PRINT_DENSITY, {density});

  sendCommand(command);
}

void sendStartLabelPrintDataExchange()
{
  PrinterCommand command = createCommand(PrinterCommands::START_LABEL_PRINT_DATA_EXCHANGE, {0x00, 0x01});
  printing = true;

  sendCommand(command);
}

void sendPrintDimensions(uint8_t width, uint8_t height)
{
  PrinterCommand command = createCommand(PrinterCommands::SET_PRINT_DIMENSIONS, {0x00, width, 0x01, height, 0x00, 0x01});

  sendCommand(command);
}

void sendEndLabelPrintDataExchange()
{
  PrinterCommand command = createCommand(PrinterCommands::END_LABEL_PRINT_DATA_EXCHANGE, {0x01});

  sendCommand(command);
}

void sendEndPrint()
{
  PrinterCommand command = createCommand(PrinterCommands::END_PRINT, {0x01});
  printing = false;

  sendCommand(command);
}

void queuePrintWhitespace(uint8_t startPosition, uint8_t thickness)
{
  printerCommands.push(
      createCommand(PrinterCommands::PRINT_WHITESPACE, {0x00, startPosition, thickness}));
}

void queuePrintLine(uint8_t startPosition, uint8_t thickness, const PrinterCommand &bodySeq)
{
  PrinterCommand positionSeq = {
      0x00, startPosition,
      0x80, 0x32,
      0x00, thickness};

  PrinterCommand command = {};

  command.insert(command.end(), positionSeq.begin(), positionSeq.end());
  command.insert(command.end(), bodySeq.begin(), bodySeq.end());

  printerCommands.push(
      createCommand(PrinterCommands::PRINT_LINE, command));
}

void queuePrint()
{
  // Print data
  queuePrintWhitespace(0, 32);
  queuePrintLine(32, 1, {0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(33, 1, {0b00000000, 0b11100000, 0b00011111, 0b00000000, 0b00000001, 0b10000000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b00011111, 0b11111000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(34, 1, {0b00000000, 0b11110000, 0b00011111, 0b00000000, 0b00000011, 0b11000000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b01111111, 0b11111110, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(35, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00001111, 0b11110000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(36, 1, {0b00000000, 0b11111100, 0b00011111, 0b00000000, 0b00001100, 0b00110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b11111110, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00111110, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(37, 1, {0b00000000, 0b11111110, 0b00011111, 0b00000000, 0b00011100, 0b00111000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(38, 1, {0b00000000, 0b11111111, 0b00011111, 0b00000000, 0b00111000, 0b00011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(39, 1, {0b00000000, 0b11111111, 0b10011111, 0b00000000, 0b00111000, 0b00011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(40, 1, {0b00000000, 0b11111011, 0b11111111, 0b00000000, 0b00111001, 0b10011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(41, 1, {0b00000000, 0b11111001, 0b11111111, 0b00000000, 0b00111111, 0b11111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(42, 1, {0b00000000, 0b11111000, 0b11111111, 0b00000000, 0b01111111, 0b11111110, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(43, 1, {0b00000000, 0b11111000, 0b01111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11100000, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(44, 1, {0b00000000, 0b11111000, 0b00111111, 0b00000000, 0b11111100, 0b00111111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(45, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00011110, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011110, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(46, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(47, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111100, 0b00111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(48, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(49, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  queuePrintLine(50, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
}

void processNextPrintingQueueLine()
{
  if (printerCommands.empty())
  {
    Serial.println("Printing queue empty");
    sendEndLabelPrintDataExchange();

    delay(1000); // TODO: Get print status before ending the print
    sendEndPrint();
    return;
  }

  PrinterCommand &command = printerCommands.front();

  Serial.print("->");
  printHexData(command.data(), command.size());
  Serial.println();

  sendCommand(command);

  printerCommands.pop();
}

class AdvertisedPrinterDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    if (advertisedDevice.getName() != PRINTER_DEVICE_NAME)
    {
      return;
    }

    advertisedDevice.getScan()->stop();
    printerDeviceAddress = new BLEAddress(advertisedDevice.getAddress());
    attemptConnectionToPrinter = true;

    Serial.println("Printer found, connecting...");
  }
};

static void printerDataNotifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *data, size_t length, bool isNotify)
{
  Serial.print("<-");
  printHexData(data, length);
  Serial.println();
}

bool connectToPrinter(BLEAddress pAddress)
{
  BLEClient *pClient = BLEDevice::createClient();

  pClient->connect(pAddress);
  Serial.println(" - Connected to Niimbot printer");

  BLERemoteService *pRemoteService = pClient->getService(niimbotB1ServiceUUID);

  if (pRemoteService == nullptr)
  {
    Serial.print("Failed to find our service UUID: ");
    Serial.println(niimbotB1ServiceUUID.toString().c_str());
    return false;
  }

  printerCommunicationCharacteristic = pRemoteService->getCharacteristic(printerCommunicationCharacteristicUUID);

  if (printerCommunicationCharacteristic == nullptr)
  {
    Serial.print("Failed to find our characteristic UUID");
    return false;
  }

  Serial.println(" - Found printer communication characteristic");

  printerCommunicationCharacteristic->registerForNotify(printerDataNotifyCallback);
  return true;
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting Niimbot proxy...");

  BLEDevice::init("B1-G121131121");

  // Setting up communication with the printer device
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedPrinterDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(30);

  while (!connectToPrinter(*printerDeviceAddress))
  {
    continue;
  }

  sendSetLabelType();
  sendSetDensity(3);

  sendGetPrintStatus();

  sendStartLabelPrintDataExchange();
  sendPrintDimensions(240, 128);

  queuePrint();
}

void loop()
{
  if (printing)
  {
    processNextPrintingQueueLine();
    return;
  }

  sendHeartbeatSignal();
  delay(1000);
}
