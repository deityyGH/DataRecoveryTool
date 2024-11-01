#include "DriveHandler.h"
#include "FAT32Recovery.h"
#include "PhysicalDriveReader.h"
#include "LogicalDriveReader.h"
#include <cwctype>
#include <iostream>
#include <algorithm>

/*=============== Private Interface ===============*/


/*=============== Core Drive Operations ===============*/
// Initialize sector reader based on drive type
void DriveHandler::initializeSectorReader() {
    // It is okay to use LogicalDriveReader, as they only differ in partition offset value, which is not important when reading the first few sectors.
    if (driveType == DriveType::LOGICAL_DRIVE) {
        auto driveReader = std::make_unique<LogicalDriveReader>(config.drivePath);
        setSectorReader(std::move(driveReader));
    }
    else if (driveType == DriveType::PHYSICAL_DRIVE) {
        auto driveReader = std::make_unique<PhysicalDriveReader>(config.drivePath, 0);
        setSectorReader(std::move(driveReader));
    }
    else {
        throw std::runtime_error("Could not determine the type of the drive (Physical, Logical). Please make sure to enter the correct drive.");
    }

    if (sectorReader == nullptr) {
        throw std::runtime_error("Failed to initialize Sector Reader.");
    }
}
// Read data from specified sector
void DriveHandler::readSector(uint64_t sector, void* buffer, uint32_t size) {
    if (!sectorReader->readSector(sector, buffer, size)) {
        throw std::runtime_error("Cannot read sector.");
    }
}
// Set the sector reader implementation
void DriveHandler::setSectorReader(std::unique_ptr<SectorReader> reader) {
    sectorReader = std::move(reader);
}
// Get bytes per sector for the drive
void DriveHandler::getBytesPerSector() {
    
    if (!sectorReader->getBytesPerSector(bytesPerSector)) {
        throw std::runtime_error("Failed to read BytesPerSector value using DeviceIoControl");
    }
}
// Close drive handle before recovery
void DriveHandler::closeDrive() {
    if (sectorReader) {
        sectorReader.reset();
    }
}


/*=============== Drive and Partition Analysis ===============*/
// Determine if drive is logical or physical
DriveType DriveHandler::determineDriveType(const std::wstring& drivePath) {
    std::wstring upperDrivePath = drivePath;
    std::transform(upperDrivePath.begin(), upperDrivePath.end(), upperDrivePath.begin(), std::towupper);

    std::wstring path = L"\\\\.\\";

    // Open Physical Drive with only a number
    if (drivePath.size() == 1 && std::isdigit(drivePath[0])) {
        config.drivePath = path + L"PhysicalDrive" + drivePath;
        
        return DriveType::PHYSICAL_DRIVE;
    }
    // Case-insensitive, find PhysicalDrive and a number in the input
    else if (upperDrivePath.find(L"PHYSICALDRIVE") != std::wstring::npos &&
        std::isdigit(upperDrivePath[upperDrivePath.size() - 1])) {
        config.drivePath = path + L"PhysicalDrive" + upperDrivePath[upperDrivePath.size() - 1];
        return DriveType::PHYSICAL_DRIVE;
    }
    // Case-insensitive, colon not necessary
    else if ((upperDrivePath.size() == 1 && std::iswalpha(upperDrivePath[0])) ||
        (upperDrivePath.size() == 2 && std::iswalpha(upperDrivePath[0]) && upperDrivePath[1] == L':')) {
        config.drivePath = path + upperDrivePath + (upperDrivePath.size() == 1 ? L":" : L"");
        return DriveType::LOGICAL_DRIVE;
    }

    return DriveType::UNKNOWN_DRIVE;
}
// Get partition scheme (MBR or GPT)
PartitionScheme DriveHandler::getPartitionScheme() {
    readMBR();
    readGPT();

    if (isGPT()) {
        return PartitionScheme::GPT_SCHEME;
    }
    else if (isMBR()) {
        return PartitionScheme::MBR_SCHEME;
    }
    
    return PartitionScheme::UNKNOWN_SCHEME;
}
// Read and parse boot sector
void DriveHandler::readBootSector(uint32_t sector) {
    readSector(sector, &bootSector, 512);

    fatStartSector = bootSector.ReservedSectorCount;
    dataStartSector = fatStartSector + (bootSector.NumFATs * bootSector.FATSize32);
    rootDirCluster = bootSector.RootCluster;

    //printBootSector();
}


/*=============== MBR Partition Handling ===============*/
// Read Master Boot Record
void DriveHandler::readMBR() {
    readSector(0, &mbr, sizeof(MBRHeader));
}
// Check if drive uses MBR
bool DriveHandler::isMBR() {
    return mbr.signature == 0xAA55;
}
// Get list of MBR partitions
void DriveHandler::getMBRPartitions() {
    for (int i = 0; i < 4; i++) {
        if (mbr.PartitionTable[i].TotalSectors != 0) {
            partitionsMBR.push_back(mbr.PartitionTable[i]);
        }
    }
}


/*=============== GPT Partition Handling ===============*/
// Read GUID Partition Table
void DriveHandler::readGPT() {
    readSector(1, &gpt, sizeof(GPTHeader));
}
// Check if drive uses GPT
bool DriveHandler::isGPT() {
    const char gptSignature[] = "EFI PART";
    return std::memcmp(gpt.Signature, gptSignature, sizeof(gptSignature) - 1) == 0;
}
// Get list of GPT partitions
void DriveHandler::getGPTPartitions() {
    uint32_t sizeOfEntry = gpt.SizeOfEntry;
    const uint32_t entriesPerSector = bytesPerSector / sizeOfEntry;
    uint8_t* sectorBuffer = new uint8_t[bytesPerSector];

    for (uint32_t i = 0; i < gpt.NumberOfEntries; i += entriesPerSector) {
        // Read full sector
        uint64_t currentLBA = gpt.PartitionEntryLBA + (i * sizeOfEntry) / bytesPerSector;
        readSector(currentLBA, sectorBuffer, bytesPerSector);

        // Process each entry in the sector
        for (uint32_t j = 0; j < entriesPerSector && (i + j) < gpt.NumberOfEntries; j++) {
            GPTPartitionEntry partitionEntry;
            std::memcpy(&partitionEntry, sectorBuffer + (j * sizeOfEntry), sizeOfEntry);

            // Check if partition is not empty
            bool isEmptyGUID = std::all_of(
                partitionEntry.PartitionTypeGUID,
                partitionEntry.PartitionTypeGUID + 16,
                [](uint8_t byte) { return byte == 0; }
            );

            if (!isEmptyGUID) {
                partitionsGPT.push_back(partitionEntry);
            }
        }
    }
    delete[] sectorBuffer;
}


/*=============== Filesystem Detection ===============*/
// Detect filesystem from boot sector
FilesystemType DriveHandler::getFSTypeFromBootSector(uint32_t sector) {
    readBootSector(sector);
    std::string fsType(reinterpret_cast<const char*>(bootSector.FileSystemType), 8);
    fsType = fsType.substr(0, fsType.find_first_of(" \0"));
    if (fsType == "FAT32") {
        return FilesystemType::FAT32_TYPE;
    }
    else if (fsType == "NTFS") {
        return FilesystemType::NTFS_TYPE;
    }
    else if (fsType == "exFAT") {
        return FilesystemType::EXFAT_TYPE;
    }
    else if (fsType == "EXT4") {
        return FilesystemType::EXT4_TYPE;
    }

    return FilesystemType::UNKNOWN_TYPE;
}
// Detect filesystem from MBR partition type
FilesystemType DriveHandler::getFSTypeFromMBRPartition(uint8_t type) {
    return static_cast<FilesystemType>(type);
}
// Detect filesystem from GPT partition GUID
FilesystemType DriveHandler::getFSTypeFromGPTPartition(const uint8_t partitionTypeGUID[16]) {
    if (std::memcmp(partitionTypeGUID, GUID_FAT32_TYPE, 16) == 0) {
        return FilesystemType::FAT32_TYPE;
    }
    return FilesystemType::UNKNOWN_TYPE;
}


/*=============== Recovery Methods ===============*/
// Recover files from logical FAT32 drive
void DriveHandler::recoverFromLogicalDriveFAT32() {
    FAT32Recovery recovery(config);

    auto driveReader = std::make_unique<LogicalDriveReader>(config.drivePath);
    recovery.setSectorReader(std::move(driveReader));

    recovery.scanForDeletedFiles(0);
    recovery.recoverAllFiles();
}
// Entry point for physical drive FAT32 recovery
void DriveHandler::recoverFromPhysicalDriveFAT32(PartitionScheme partitionScheme) {
    if (partitionScheme == PartitionScheme::MBR_SCHEME) {
        for (const auto& partition : partitionsMBR) {
            FilesystemType fsType = getFSTypeFromMBRPartition(partition.Type);
            if (fsType == FilesystemType::FAT32_TYPE) {
                recoverFromPhysicalDriveFAT32MBR(partition);

            }
            // TODO: Add more fs types
        }
        return;
    }
    else if (partitionScheme == PartitionScheme::GPT_SCHEME) {
        for (const auto& partition : partitionsGPT) {
            FilesystemType fsType = getFSTypeFromGPTPartition(partition.PartitionTypeGUID);
            if (fsType == FilesystemType::FAT32_TYPE) {
                recoverFromPhysicalDriveFAT32GPT(partition);
            }
        }
        return;
    }
    throw std::runtime_error("Partition is neither MBR nor GPT. Please make sure to enter the correct drive.");
}
// Recover FAT32 files from MBR partition
void DriveHandler::recoverFromPhysicalDriveFAT32MBR(const MBRPartitionEntry& partition) {
    FAT32Recovery recovery(config);
    uint64_t startLBA = partition.StartLBA;

    auto driveReader = std::make_unique<PhysicalDriveReader>(config.drivePath, startLBA);
    recovery.setSectorReader(std::move(driveReader));

    recovery.scanForDeletedFiles(0);
    recovery.recoverAllFiles();
}
// Recover FAT32 files from GPT partition
void DriveHandler::recoverFromPhysicalDriveFAT32GPT(const GPTPartitionEntry& partition) {
    FAT32Recovery recovery(config);
    uint64_t startLBA = partition.StartingLBA;

    auto driveReader = std::make_unique<PhysicalDriveReader>(config.drivePath, startLBA);
    recovery.setSectorReader(std::move(driveReader));

    recovery.scanForDeletedFiles(0);
    recovery.recoverAllFiles();
}


/*=============== Debug Output Methods ===============*/
// Print MBR contents
void DriveHandler::printMBR() {
    std::cout << "MBR Boot Code (First 16 bytes): ";
    for (int i = 0; i < 16; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(mbr.bootCode[i]) << " ";
    }
    std::cout << "\n";

    // Print each partition entry
    for (int i = 0; i < 4; i++) {
        printMBRPartitionEntry(mbr.PartitionTable[i], i);
    }

    // Print the signature
    std::cout << "MBR Signature: 0x" << std::hex << mbr.signature << "\n";
}
// Print single MBR partition entry
void DriveHandler::printMBRPartitionEntry(const MBRPartitionEntry& entry, int index) {

    std::cout << "Partition Entry " << index + 1 << ":\n";
    std::cout << "  Boot Flag:      " << (entry.BootIndicator == 0x80 ? "Bootable" : "Not Bootable") << "\n";
    std::cout << "  Start CHS:      "
        << static_cast<int>(entry.StartHead) << "/"
        << static_cast<int>(entry.StartSector) << "/"
        << static_cast<int>(entry.StartCylinder) << "\n";
    std::cout << "  Type:           " << std::hex << "0x" << static_cast<int>(entry.Type) << "\n";
    std::cout << "  End CHS:        "
        << static_cast<int>(entry.EndHead) << "/"
        << static_cast<int>(entry.EndSector) << "/"
        << static_cast<int>(entry.EndCylinder) << "\n";
    std::cout << "  Start LBA:      " << entry.StartLBA << "\n";
    std::cout << "  Number of Sectors: " << entry.TotalSectors << "\n";
    std::cout << "--------------------------------------\n";
}
// Print array in hex format
void DriveHandler::printHexArray(const uint8_t* array, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(array[i]) << " ";
    }
    std::cout << std::dec << std::endl;  // Reset to decimal format
}
// Print boot sector contents
void DriveHandler::printBootSector() {
    std::cout << "Boot Sector Values:\n";
    std::cout << "jmpBoot: ";
    printHexArray(bootSector.jmpBoot, 3);

    std::cout << "OEMName: ";
    std::cout.write(reinterpret_cast<const char*>(bootSector.OEMName), 8);
    std::cout << std::endl;

    std::cout << "BytesPerSector: " << bootSector.BytesPerSector << std::endl;
    std::cout << "SectorsPerCluster: " << static_cast<int>(bootSector.SectorsPerCluster) << std::endl;
    std::cout << "ReservedSectorCount: " << bootSector.ReservedSectorCount << std::endl;
    std::cout << "NumFATs: " << static_cast<int>(bootSector.NumFATs) << std::endl;
    std::cout << "RootEntryCount: " << bootSector.RootEntryCount << std::endl;
    std::cout << "TotalSectors16: " << bootSector.TotalSectors16 << std::endl;
    std::cout << "Media: " << static_cast<int>(bootSector.Media) << std::endl;
    std::cout << "FATSize16: " << bootSector.FATSize16 << std::endl;
    std::cout << "SectorsPerTrack: " << bootSector.SectorsPerTrack << std::endl;
    std::cout << "NumberOfHeads: " << bootSector.NumberOfHeads << std::endl;
    std::cout << "HiddenSectors: " << bootSector.HiddenSectors << std::endl;
    std::cout << "TotalSectors32: " << bootSector.TotalSectors32 << std::endl;
    std::cout << "FATSize32: " << bootSector.FATSize32 << std::endl;
    std::cout << "ExtFlags: " << bootSector.ExtFlags << std::endl;
    std::cout << "FSVersion: " << bootSector.FSVersion << std::endl;
    std::cout << "RootCluster: " << bootSector.RootCluster << std::endl;
    std::cout << "FSInfo: " << bootSector.FSInfo << std::endl;
    std::cout << "BkBootSec: " << bootSector.BkBootSec << std::endl;

    std::cout << "Reserved: ";
    printHexArray(bootSector.Reserved, 12);

    std::cout << "DriveNumber: " << static_cast<int>(bootSector.DriveNumber) << std::endl;
    std::cout << "Reserved1: " << static_cast<int>(bootSector.Reserved1) << std::endl;
    std::cout << "BootSignature: " << static_cast<int>(bootSector.BootSignature) << std::endl;
    std::cout << "Boot sector signature: 0x" << std::hex << bootSector.BootSectorSignature << std::dec << std::endl;

    std::cout << "VolumeID: " << bootSector.VolumeID << std::endl;

    std::cout << "VolumeLabel: ";
    std::cout.write(reinterpret_cast<const char*>(bootSector.VolumeLabel), 11);
    std::cout << std::endl;

    std::cout << "FileSystemType: ";
    std::cout.write(reinterpret_cast<const char*>(bootSector.FileSystemType), 8);
    std::cout << std::endl;
    printHexArray(bootSector.FileSystemType, 8);
}
// Print GPT header information
void DriveHandler::printGPTHeader() {
    std::cout << "=== GPT Header ===" << std::endl;
    std::cout << "Signature: " << std::string(reinterpret_cast<const char*>(gpt.Signature), 8) << std::endl;
    std::cout << "Revision: " << std::hex << std::showbase << gpt.Revision << std::endl;
    std::cout << "Header Size: " << std::dec << gpt.HeaderSize << " bytes" << std::endl;
    std::cout << "Header CRC32: " << std::hex << std::showbase << gpt.HeaderCRC32 << std::endl;
    std::cout << "Current LBA: " << gpt.CurrentLBA << std::endl;
    std::cout << "Backup LBA: " << gpt.BackupLBA << std::endl;
    std::cout << "First Usable LBA: " << gpt.FirstUsableLBA << std::endl;
    std::cout << "Last Usable LBA: " << gpt.LastUsableLBA << std::endl;
    std::cout << "Disk GUID: " << guidToString(gpt.DiskGUID) << std::endl;
    std::cout << "Partition Entry LBA: " << gpt.PartitionEntryLBA << std::endl;
    std::cout << "Number of Partition Entries: " << gpt.NumberOfEntries << std::endl;
    std::cout << "Size of Partition Entry: " << gpt.SizeOfEntry << " bytes" << std::endl;
    std::cout << "Partition Entry Array CRC32: " << std::hex << std::showbase << gpt.PartitionEntryArrayCRC32 << std::endl;
    std::cout << "==================" << std::endl;
}
// Print single GPT partition entry
void DriveHandler::printGPTEntry(const GPTPartitionEntry& entry) {
    std::cout << "=== GPT Partition Entry ===" << std::endl;
    std::cout << "Partition Type GUID: " << guidToString(entry.PartitionTypeGUID) << std::endl;
    std::cout << "Unique Partition GUID: " << guidToString(entry.UniquePartitionGUID) << std::endl;
    std::cout << "Starting LBA: " << entry.StartingLBA << std::endl;
    std::cout << "Ending LBA: " << entry.EndingLBA << std::endl;
    std::cout << "Attributes: " << std::hex << std::showbase << entry.Attributes << std::endl;
    std::cout << "Partition Name: " << utf16ToString(entry.PartitionName, 36) << std::endl;
    std::cout << "===========================" << std::endl;
}


/*=============== String Conversion Utilities ===============*/
// Convert GPT GUID to string format
std::string DriveHandler::guidToString(const uint8_t guid[16]) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(2) << static_cast<int>(guid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            ss << "-";
        }
    }
    return ss.str();
}
// Convert UTF-16 GPT partition name to string
std::string DriveHandler::utf16ToString(const uint16_t utf16Str[], size_t size) {
    std::wstring wstr(utf16Str, utf16Str + size);
    std::string str(wstr.begin(), wstr.end()); // Convert to std::string from wstring
    return str;
}


/*=============== Public Interface ===============*/
// Constructor
DriveHandler::DriveHandler(const Config& cfg)
    : config(cfg), driveType(DriveType::UNKNOWN_DRIVE)
{

    driveType = determineDriveType(config.drivePath);
    initializeSectorReader();


  
}
// Main recovery entry point
void DriveHandler::recoverDrive() {
    if (driveType == DriveType::LOGICAL_DRIVE) {
        FilesystemType fsType = getFSTypeFromBootSector(0);
        closeDrive();
        if (fsType == FilesystemType::FAT32_TYPE) {
            recoverFromLogicalDriveFAT32();
        }
    }
    else if (driveType == DriveType::PHYSICAL_DRIVE) {
        PartitionScheme partitionScheme = getPartitionScheme();
        if (partitionScheme == PartitionScheme::MBR_SCHEME) {
            getMBRPartitions();
        }
        else if (partitionScheme == PartitionScheme::GPT_SCHEME) {
            getBytesPerSector();
            getGPTPartitions();
        }
        else { return; }

        closeDrive();
        recoverFromPhysicalDriveFAT32(partitionScheme);
    }
}


