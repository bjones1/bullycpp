/** This file is part of BullyCPP.
 **
 **     BullyCPP is free software: you can redistribute it and/or modify
 **     it under the terms of the GNU General Public License as published by
 **     the Free Software Foundation, either version 3 of the License, or
 **     (at your option) any later version.
 **
 **     BullyCPP is distributed in the hope that it will be useful,
 **     but WITHOUT ANY WARRANTY; without even the implied warranty of
 **     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **     GNU General Public License for more details.
 **
 **     You should have received a copy of the GNU General Public License
 **     along with BullyCPP.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include "PicBootloaderDriver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sstream>

#include "util.h"

namespace bullycpp {

PicBootloaderDriver::PicBootloaderDriver(ISerialPort& port, IProgressCallback* progressCallback)
	: port(port)
	, progressCallback(progressCallback)
	, configBitsEnabled(true)
	, firmwareVersion(0)
	, currentDevice()
{}

const PicDevice* PicBootloaderDriver::readDevice()
{
	std::array<uint8_t, 8> inputData;
	uint16_t deviceID = 0, revision = 0, processID = 0;

	giveProgress(Status::Busy, 0);

	this->port.clear();

	this->port << Command::READ_ID;
	this->port >> inputData;

	deviceID = (inputData[1] << 8) | inputData[0];
	processID = inputData[5] >> 4;
	revision = (inputData[5] << 8) | inputData[4];

	giveProgress(Status::Idle, 0);

	for(auto& device: this->devices) {
		if(device.id == deviceID && device.processID == processID) {
			// found device
			this->currentDevice = &device;
			this->currentDevice->revision = revision;
			return this->currentDevice;
		}
	}

	this->currentDevice = nullptr;
	std::cerr << "Device ID: 0x" << std::hex << deviceID << "\n"
	          << "Process ID: 0x" << std::hex << processID << "\n"
	          << "Refusing to program unknown device.  Check device or baud rate."
	          << std::endl;

	return this->currentDevice;
}

void PicBootloaderDriver::setMCLR(bool mclr)
{
	this->port.setRTS(mclr);
	this->port.setDTR(mclr);
}

bool PicBootloaderDriver::shouldSkipRow(const MemRow& thisRow, const PicDevice::Family family)
{
	uint32_t i = thisRow.getAddress();

	if(this->firmwareVersion >= 3 && i < PROGRAM_START)
		// Do not write any rows below program start
		return true;

	if(family == PicDevice::Family::PIC24F ||
	   family == PicDevice::Family::PIC24E ||
	   family == PicDevice::Family::dsPIC33E) {
		// Check if this is the config bit page
		if(i >= currentDevice->configPage && !thisRow.isEmpty()) {
			std::cout << "Skipping memory row " << std::hex << i << " on config bit page" << std::endl;

			return true;
		}
	}
	return false;
}

void PicBootloaderDriver::getVersion() {
	uint8_t majorVersion;

	if(!this->currentDevice)
		throw std::logic_error("Device not read, or unknown device");

	std::cout << "Reading firmware version...\n";
	this->port << Command::READ_VERSION;
	this->port >> majorVersion;

	if(majorVersion == Command::NACK) {
		// Old bootloader
		this->firmwareVersion = 0;

		if(!currentDevice)
			throw std::logic_error("Device not read!");
		this->configBitsEnabled = (currentDevice->family == PicDevice::Family::PIC24H ||
		                           currentDevice->family == PicDevice::Family::PIC24FK ||
		                           currentDevice->family == PicDevice::Family::dsPIC33F);

		std::cerr << "Detected firmware version 0: Config bits always written for PIC24H,\n"
		          << "but not for PIC24F, PIC24E, or dsPIC33E (last page of program\n"
		          << "memory skipped for these devices).\n"
		          << "Update to the latest firmware to change this behavior." << std::endl;
		return;
	}

	this->firmwareVersion = majorVersion;

	uint8_t minorVersion, ack;
	this->port >> minorVersion >> ack;

	if(ack != Command::ACK) {
		return;
	}

	std::cout << "Firmware version: "
	          << static_cast<unsigned int>(majorVersion) << "." << static_cast<unsigned int>(minorVersion)
	          << ", config bits programming " << (this->configBitsEnabled ? "enabled." : "disabled.")
	          << std::endl;

	if(this->firmwareVersion >= 3) {
		std::cout << "Firmware v3.0 or later detected.\n"
		          << "No pages below location 0x" << std::hex << PROGRAM_START << " will be written." << std::endl;
	}
}

void PicBootloaderDriver::programHexFile(const std::string& path) {
	std::ifstream hexFile(path);
	programHexFile(hexFile);
}

void PicBootloaderDriver::programHexFile(std::istream& hexFile)
{
	std::vector<uint8_t> buffer;
	int32_t extAddr = 0;
	std::vector<MemRow> ppMemory;
	uint32_t pm33f_rowsize;

	getVersion();

	const PicDevice::Family family = this->currentDevice->family;
	if(family == PicDevice::Family::PIC24FK)
		pm33f_rowsize = MemRow::PIC24FK_ROW_SIZE;
	else if(this->currentDevice->smallRAM)
		pm33f_rowsize = MemRow::PM33F_ROW_SIZE_SMALL;
	else
		pm33f_rowsize = MemRow::PM33F_ROW_SIZE_LARGE;

	ppMemory.reserve(MemRow::PM_SIZE + MemRow::EE_SIZE + MemRow::CM_SIZE);

	for(unsigned int row = 0; row < MemRow::PM_SIZE; ++row)
		ppMemory.emplace_back(MemRow::MemType::Program, 0x000000, row, family, pm33f_rowsize);
	for(unsigned int row = 0; row < MemRow::EE_SIZE; ++row)
		ppMemory.emplace_back(MemRow::MemType::EEProm, 0x7FF000, row, family, pm33f_rowsize);
	for(unsigned int row = 0; row < MemRow::CM_SIZE; ++row)
		ppMemory.emplace_back(MemRow::MemType::Configuration, 0xF80000, row, family, pm33f_rowsize);

	std::cout << "Reading hex file..." << std::endl;

	if(!hexFile) {
		std::cerr << "Error while opening hex file." << std::endl;
		return;
	}

	while(hexFile.good()) {
		std::string line;
		std::getline(hexFile, line);

		if(line.empty())
			continue;

		std::istringstream lineStream(std::move(line));

		lineStream.ignore(1); // Ignore ':'
		const uint8_t byteCount = parseHex<uint8_t>(lineStream);
        // Although the raw address here is 16 bits, the extended address (extAddr) allows an effective address with 32-bit range. Therefore, store the address as 32 bits.
		uint32_t address = parseHex<uint16_t>(lineStream);
		const uint8_t recordType = parseHex<uint8_t>(lineStream);
		switch(recordType) {
		case 0:
			address = (address + extAddr) / 2;
			if(!checkAddressClash(address, family)) {
				std::cerr << "Program address in hex file clashes with bootloader location.\n"
				          << "Aborting.  Recompile target code with appropriate linker file."
				          << std::endl;
				return;
			}
			for(uint8_t charCount = 0; charCount < byteCount * 2; charCount += 4, ++address) {
				bool inserted = false;
				uint16_t data = parseHex<uint16_t>(lineStream);
				for(auto& row: ppMemory) {
					if(!checkAddressClash(address, data, family)) {
						std::cerr << "Program data in hex file clashes with bootloader.\n"
						          << "Aborting.  Recompile target code with appropriate linker file."
						          << std::endl;
						return;
					}
					else if(!checkAddressClash(address, data, family, currentDevice->configPage, currentDevice->configWord)) {
						std::cerr << "Configuration bit programming is not enabled,\n"
						          << "but data exists on the last page of flash!\n"
						          << "Aborting.  Enable config bit programming or change hex file."
						          << std::endl;
						return;
					}

					if( (inserted = row.insertData(address, data)) )
						break;
				}
				if(!inserted) {
					std::cerr << "Bad hex file: 0x" << std::hex << address << " out of range." << std::endl;
					return;
				}
			}
			break;
		case 1:
			// Do nothing
			break;
		case 4:
			extAddr = parseHex<uint16_t>(lineStream) << 16;
			break;
		default:
			std::cerr << "Error: unknown hex record type 0x" << std::hex << recordType << std::endl;
			return;
		}
	}

	std::cout << "Hex file read successfully." << std::endl;

	if(this->firmwareVersion < 3) {
		// Preserve first two locations for bootloader
		size_t rowSize;

		if(family == PicDevice::Family::dsPIC30F)
			rowSize = MemRow::PM30F_ROW_SIZE;
		else if(currentDevice->family == PicDevice::Family::PIC24FK)
			rowSize = MemRow::PIC24FK_ROW_SIZE;
		else if(currentDevice->smallRAM)
			rowSize = MemRow::PM33F_ROW_SIZE_SMALL;
		else
			rowSize = MemRow::PM33F_ROW_SIZE_LARGE;

		std::vector<uint8_t> data(rowSize * 3);

		this->port << Command::READ_PM << 0x00 << 0x00 << 0x00;
		this->port >> data;

		throw std::logic_error("TODO: I have no idea what's going on here.");
	}

	int nonEmptyProgramRows = 0, nonEmptyProgramRowCount = 0;
	int nonEmptyRows = 0, nonEmptyRowCount = 0;

	// Format data and count nonempty rows
	for(auto& row: ppMemory) {
		row.formatData();
		if(!row.isEmpty()) {
			++nonEmptyRows;
			if(row.getType() == MemRow::MemType::Program)
				++nonEmptyProgramRows;
		}
	}

	// Keep a copy for verification
	std::vector<MemRow> ppMemoryVerify(ppMemory);

	std::cout << "Programming device..." << std::endl;
	for(const auto& row: ppMemory) {
		if(!row.isEmpty())
			giveProgress(Status::Programming, 100 * ++nonEmptyRowCount / nonEmptyRows);
		if(row.getType() == MemRow::MemType::Configuration && !this->configBitsEnabled)
			continue;
		if(!shouldSkipRow(row, family))
			row.sendData(this->port);
		if(row.getType() == MemRow::MemType::Configuration
		   && row.getRowNumber() == 0
		   && family == PicDevice::Family::PIC24H)
			std::cout << "Config bits sent." << std::endl;
	}

	std::cout << "\nVerifying..." << std::endl;

	bool verifyOK = true;

	// only verify program memory
	for(unsigned int row = 0; row < MemRow::PM_SIZE; ++row) {
		if(!ppMemory[row].isEmpty()) {
			giveProgress(Status::Verifying, 100 * ++nonEmptyProgramRowCount / nonEmptyProgramRows);
		}
		if(shouldSkipRow(ppMemory[row], family))
			continue;
		if(ppMemory[row].readData(this->port)) {
			uint32_t address = ppMemory[row].getAddress();
			for(unsigned int index = 0; index < ppMemory[row].getRowSize(); ++index, address += 2) {
				const uint32_t expected = (ppMemoryVerify[row].getByte(3 * index + 2) << 16) +
				                          (ppMemoryVerify[row].getByte(3 * index + 1) << 8) +
				                          (ppMemoryVerify[row].getByte(3 * index));
				const uint32_t got = (ppMemory[row].getByte(3 * index) << 16) +
				                     (ppMemory[row].getByte(3 * index + 1) << 8) +
				                     (ppMemory[row].getByte(3 * index + 2));

				if(expected != got) {
					verifyOK = false;
					std::cerr << "Verification failed at address 0x" << std::hex << address << "!\n"
					          << "Expected 0x" << std::hex << expected << ", got 0x" << std::hex << got
					          << std::endl;
					break;
				}
			}
		}
		else {
			std::cerr << "Problem reading program memory during verification." << std::endl;
		}
		if(!verifyOK)
			break;
	}
	if(!verifyOK) {
		std::cerr << "Verification failed." << std::endl;
		giveProgress(Status::Error, 0);
	}

	// Because of the way the firmware is written, we need to resend the config bytes
	// before a reset (if programming the config bits)

	if(this->configBitsEnabled) {
		for(const auto& row: ppMemory) {
			if(row.getType() == MemRow::MemType::Configuration)
				row.sendData(this->port);
		}
	}

	if(this->firmwareVersion == 0 || this->configBitsEnabled)
		this->port << Command::RESET;
	else
		this->port << Command::POR_RESET;

	std::cout << "Done!" << std::endl;
	if(verifyOK)
		giveProgress(Status::Idle, 100);
}

void PicBootloaderDriver::parseDeviceFile(const std::string& path)
{
	std::ifstream deviceFile(path);
	parseDeviceFile(deviceFile);
}

void PicBootloaderDriver::parseDeviceFile(std::istream& deviceFile)
{
	using namespace std;
	string line;
	while(deviceFile.good()) {
		getline(deviceFile, line);
		// Trim line
		size_t pos = line.find_last_not_of(" \t\n");
		if(pos != string::npos)
			line = line.substr(0, pos + 1);
		pos = line.find_first_not_of(" \t\n");
		if(pos != string::npos)
			line = line.substr(pos);
		if(!line.empty() && line[0] != '#')
			parseDeviceLine(line);
	}
}

void PicBootloaderDriver::parseDeviceLine(const std::string& deviceLine)
{
	using std::string;
	using std::stoi;
	using std::get;

	std::vector<string> parts;
	parts.reserve(6);

	// Split deviceLine on ','s
	std::stringstream ss(deviceLine);
	std::string item;
	while(std::getline(ss, item, ','))
		parts.push_back(item);

	if(parts.size() != 6) {
		std::cerr << "Bad device line: " << deviceLine << std::endl;
		return;
	}

	try {
		const string& devName = parts[0];
		const uint32_t devID = stoi(parts[1], 0, 16);
		const uint32_t PID = stoi(parts[2]);
		const string& famName = parts[3];
		const uint32_t configPage = stoi(parts[4], 0, 16);
		const bool smallRAM = (stoi(parts[5]) != 0);

		static const std::map<string, PicDevice::Family> map = {
			{"dsPIC30F", PicDevice::Family::dsPIC30F},
			{"dsPIC33F", PicDevice::Family::dsPIC33F},
			{"PIC24H",   PicDevice::Family::PIC24H},
			{"PIC24F",   PicDevice::Family::PIC24F},
			{"PIC24FK",  PicDevice::Family::PIC24FK},
			{"PIC24E",   PicDevice::Family::PIC24E},
			{"dsPIC33E", PicDevice::Family::dsPIC33E}
		};
		const auto familyIter = map.find(famName);
		if(familyIter == std::end(map)) {
			std::cerr << "Unrecognized device: " << famName << std::endl;
			return;
		}

		this->devices.emplace_back(devName, devID, PID, get<1>(*familyIter), configPage, smallRAM);
	} catch(std::exception& e) {
		std::cerr << "Error while parsing device line \"" << deviceLine << "\": " << e.what() << std::endl;
		return;
	}
}

namespace {
	static bool checkAddressClashFamily(const PicDevice::Family family) {
		using std::begin;
		using std::end;
		const std::vector<PicDevice::Family> families {
			PicDevice::Family::PIC24H,
			PicDevice::Family::dsPIC33F,
			PicDevice::Family::PIC24E,
			PicDevice::Family::dsPIC33E,
			PicDevice::Family::PIC24FK,
			PicDevice::Family::PIC24F
		};
		return std::find(begin(families), end(families), family) == end(families);
	}
}

bool PicBootloaderDriver::checkAddressClash(const uint32_t address, const PicDevice::Family family)
{
	// Check if page starts at 0x400.
	// If so, this page definitely clashes with the bootloader.
	return !(checkAddressClashFamily(family)
	         && (address == 0x400));
}

bool PicBootloaderDriver::checkAddressClash(const uint32_t address, const uint16_t data, const PicDevice::Family family)
{
	// Ensure that each word between >= 0x200 and <0xC00 is == 0xFFFF
	// or else we clash with bootloader!
	return !(checkAddressClashFamily(family)
	         && (address >= 0x200 && address < PROGRAM_START && data != 0xFFFF));
}

bool PicBootloaderDriver::checkAddressClash(const uint32_t address, const uint16_t data, const PicDevice::Family family, const unsigned int configPage, const unsigned int configWord)
{
	// If PIC24F/PIC24E/dsPIC33E code is located on last page,
	// and configuration bit programming is not enabled, then abort.
	if(family == PicDevice::Family::PIC24F ||
	   family == PicDevice::Family::PIC24E ||
	   family == PicDevice::Family::dsPIC33E)
	{
		if(this->configBitsEnabled)
			return true;
		if(address >= configPage && address < configWord && data != 0xFFFF)
			return false;
	}
	return true;
}

void PicBootloaderDriver::giveProgress(IProgressCallback::Status status, int percent)
{
	if(this->progressCallback)
		this->progressCallback->onProgress(status, percent);
}

}
