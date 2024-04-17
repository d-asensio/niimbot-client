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
static boolean printing = true;

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
  const uint8_t START_PRINT = 0x01;
  const uint8_t SET_PRINT_DIMENSIONS = 0x13;
  const uint8_t END_PAGE_PRINT = 0xE3;
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

void sendCalibrateLabelGapSignal()
{
  printerCommands.push(
      createCommand(PrinterCommands::CALIBRATE_LABEL_GAP, {0x01}));
}

void sendHeartbeatSignal()
{
  printerCommands.push(
      createCommand(PrinterCommands::HEARTBEAT, {0x04}));
}

void sendGetPrintStatus()
{
  PrinterCommand command = createCommand(PrinterCommands::GET_PRINT_STATUS, {0x01});

  printerCommunicationCharacteristic->writeValue(command.data(), command.size(), true);
  delay(100);
}

void sendGetRFID()
{
  printerCommands.push(
      createCommand(PrinterCommands::GET_LABEL_RFID, {0x01}));
}

void sendSetLabelType()
{
  printerCommands.push(
      createCommand(PrinterCommands::SET_LABEL_TYPE, {0x01}));
}

void sendSetDensity(uint8_t density)
{
  printerCommands.push(
      createCommand(PrinterCommands::SET_PRINT_DENSITY, {density}));
}

void sendStartPrint()
{
  printerCommands.push(
      createCommand(PrinterCommands::START_PRINT, {0x00, 0x01}));
}

void sendPrintDimensions(uint8_t width, uint8_t height)
{
  printerCommands.push(
      createCommand(PrinterCommands::SET_PRINT_DIMENSIONS, {0x00, width, 0x01, height, 0x00, 0x01}));
}

void sendEndPage()
{
  printerCommands.push(
      createCommand(PrinterCommands::END_PAGE_PRINT, {0x01}));
}

void sendEndPrint()
{
  printerCommands.push(
      createCommand(PrinterCommands::END_PRINT, {0x01}));
  printing = false;
}

void sendPrintWhitespace(uint8_t startPosition, uint8_t thickness)
{
  printerCommands.push(
      createCommand(PrinterCommands::PRINT_WHITESPACE, {0x00, startPosition, thickness}));
}

void sendPrintLine(uint8_t startPosition, uint8_t thickness, const PrinterCommand &bodySeq)
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

void sendPrint()
{
  sendGetRFID();
  sendSetLabelType();
  sendSetDensity(3);
  sendStartPrint();
  sendPrintDimensions(240, 128);

  // Print data
  // sendPrintLine(0, 10, { 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
  // sendPrintWhitespace(10, 215);
  // sendPrintLine(225, 2, { 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00 });
  // 5 pixel xor
  // for (u_int8_t i = 0; i < (8 * 4); i++)
  // {
  //   sendPrintLine(i, 1, { 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00 });
  // }

  // for (u_int8_t i = 0; i < 64; i++)
  // {
  //   sendPrintLine(i, 1, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0b00000000, 0xFF, 0x00, 0xFF, 0b00101000, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, static_cast<uint8_t>(i + 64), 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, static_cast<uint8_t>(i + 128), 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, static_cast<uint8_t>(i + 192), 0xFF, 0x00, 0x00});
  // }

  sendPrintWhitespace(0, 32);
  sendPrintLine(32, 1, {0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000,0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(33, 1, {0b00000000, 0b11100000, 0b00011111, 0b00000000, 0b00000001, 0b10000000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b00011111, 0b11111000, 0b00000000, 0b11111111, 0b11111111, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(34, 1, {0b00000000, 0b11110000, 0b00011111, 0b00000000, 0b00000011, 0b11000000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b01111111, 0b11111110, 0b00000000, 0b11111111, 0b11111111, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(35, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00001111, 0b11110000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(36, 1, {0b00000000, 0b11111100, 0b00011111, 0b00000000, 0b00001100, 0b00110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b11111110, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00111110, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(37, 1, {0b00000000, 0b11111110, 0b00011111, 0b00000000, 0b00011100, 0b00111000, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(38, 1, {0b00000000, 0b11111111, 0b00011111, 0b00000000, 0b00111000, 0b00011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(39, 1, {0b00000000, 0b11111111, 0b10011111, 0b00000000, 0b00111000, 0b00011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(40, 1, {0b00000000, 0b11111011, 0b11111111, 0b00000000, 0b00111001, 0b10011100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(41, 1, {0b00000000, 0b11111001, 0b11111111, 0b00000000, 0b00111111, 0b11111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(42, 1, {0b00000000, 0b11111000, 0b11111111, 0b00000000, 0b01111111, 0b11111110, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11100000, 0b00000000, 0b11111111, 0b11110000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(43, 1, {0b00000000, 0b11111000, 0b01111111, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11100000, 0b00000000, 0b11111000, 0b01111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(44, 1, {0b00000000, 0b11111000, 0b00111111, 0b00000000, 0b11111100, 0b00111111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00111100, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(45, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11110000, 0b00001111, 0b00000000, 0b11111000, 0b00011110, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011110, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(46, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(47, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111100, 0b00111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11110000, 0b00000000, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(48, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(49, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});
  sendPrintLine(50, 1, {0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000, 0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000111, 0b11100000, 0b00000000,0b11111111, 0b11111111, 0b00000000, 0b11111000, 0b00011111, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b00000000, 0b11111111});

  sendEndPage();
  sendEndPrint();
}

void processNextCommand()
{
  if (printerCommands.empty())
  {
    return;
  }

  PrinterCommand &command = printerCommands.front();

  Serial.print("->");
  printHexData(command.data(), command.size());
  Serial.println();

  printerCommunicationCharacteristic->writeValue(command.data(), command.size(), true);
  // TODO: Different delay for print commands than regular commands
  // TODO: Get print status before sending a print command
  delay(500);

  printerCommands.pop();
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

  sendHeartbeatSignal();
  sendPrint();
  sendGetPrintStatus();
}

void loop()
{
  if (printing)
  {
    sendGetPrintStatus();
  }

  processNextCommand();
}
