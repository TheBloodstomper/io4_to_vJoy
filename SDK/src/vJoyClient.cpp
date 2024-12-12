#include <windows.h>
#include <setupapi.h>
#include <hidclass.h>
#include <hidsdi.h>        // Important for HID functions and HIDD_ATTRIBUTES
#include <stdio.h>
#include <tchar.h>
#include <string>
#include "public.h"        // vJoy public header
#include "vjoyinterface.h" // vJoy interface functions

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// The Vendor and Product IDs for the Sega IO4 board
#define SEGA_IO4_VID 0x0CA3
#define SEGA_IO4_PID 0x0021

#pragma pack(push, 1)
struct io4_report_in {
    uint8_t report_id;    // =1
    uint16_t adcs[8];     // 8 analog values (0..65535)
    uint16_t spinners[4];
    uint16_t chutes[2];
    uint16_t buttons[2];
    uint8_t system_status;
    uint8_t usb_status;
    uint8_t unknown[29];  // not used
};
#pragma pack(pop)

// Helper function to find the IO4 device path
bool FindIO4DevicePath(std::wstring& devicePath)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    DWORD index = 0;

    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, index, &deviceInterfaceData)) {
        index++;

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &deviceInterfaceData, NULL, 0, &requiredSize, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W deviceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        if (!deviceDetailData) {
            continue;
        }
        deviceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &deviceInterfaceData, deviceDetailData, requiredSize, &requiredSize, NULL)) {
            free(deviceDetailData);
            continue;
        }

        HANDLE testHandle = CreateFileW(deviceDetailData->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (testHandle != INVALID_HANDLE_VALUE) {
            HIDD_ATTRIBUTES attrib;
            attrib.Size = sizeof(HIDD_ATTRIBUTES);
            if (HidD_GetAttributes(testHandle, &attrib)) {
                if (attrib.VendorID == SEGA_IO4_VID && attrib.ProductID == SEGA_IO4_PID) {
                    devicePath = deviceDetailData->DevicePath;
                    CloseHandle(testHandle);
                    free(deviceDetailData);
                    SetupDiDestroyDeviceInfoList(deviceInfoSet);
                    return true;
                }
            }
            CloseHandle(testHandle);
        }

        free(deviceDetailData);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return false;
}

int main(int argc, char* argv[])
{
    UINT DevID = 1; // Choose your vJoy device ID

    // Initialize vJoy
    if (!vJoyEnabled()) {
        printf("vJoy is not enabled or not installed.\n");
        return -1;
    }

    VjdStat status = GetVJDStatus(DevID);
    switch (status) {
    case VJD_STAT_OWN:
        printf("vJoy device %d is already owned by this feeder\n", DevID);
        break;
    case VJD_STAT_FREE:
        printf("vJoy device %d is free\n", DevID);
        break;
    case VJD_STAT_BUSY:
        printf("vJoy device %d is already owned by another feeder\nCannot continue\n", DevID);
        return -1;
    case VJD_STAT_MISS:
        printf("vJoy device %d is not installed or disabled\nCannot continue\n", DevID);
        return -1;
    default:
        printf("vJoy device %d general error\nCannot continue\n", DevID);
        return -1;
    };

    if (!AcquireVJD(DevID)) {
        printf("Failed to acquire vJoy device number %d.\n", DevID);
        return -1;
    }
    printf("Acquired vJoy device %d.\n", DevID);

    // Find IO4 device path
    std::wstring io4Path;
    if (!FindIO4DevicePath(io4Path)) {
        printf("Could not find Sega IO4 board.\n");
        RelinquishVJD(DevID); // Ensure vJoy is released if IO4 is not found
        return -1;
    }
    // Convert std::wstring to UTF-16 for printing
    wprintf(L"Found IO4 device: %ls\n", io4Path.c_str());

    // Open IO4 HID device
    HANDLE io4Handle = CreateFileW(io4Path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (io4Handle == INVALID_HANDLE_VALUE) {
        printf("Failed to open IO4 device.\n");
        RelinquishVJD(DevID);
        return -1;
    }

    // Setup asynchronous IO
    OVERLAPPED overlap;
    ZeroMemory(&overlap, sizeof(OVERLAPPED));
    overlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    const DWORD REPORT_SIZE = 0x40; // 64 bytes
    BYTE reportBuffer[REPORT_SIZE];

    // Continuous read loop
    while (true) {
        ResetEvent(overlap.hEvent);
        DWORD bytesRead = 0;
        if (!ReadFile(io4Handle, reportBuffer, REPORT_SIZE, &bytesRead, &overlap)) {
            if (GetLastError() == ERROR_IO_PENDING) {
                DWORD waitRes = WaitForSingleObject(overlap.hEvent, 100);
                if (waitRes == WAIT_TIMEOUT) {
                    continue;
                }
                else {
                    if (!GetOverlappedResult(io4Handle, &overlap, &bytesRead, TRUE)) {
                        printf("GetOverlappedResult failed.\n");
                        break;
                    }
                }
            }
            else {
                printf("IO4 read error.\n");
                break;
            }
        }

        // Check if the report is the expected one
        if (bytesRead == REPORT_SIZE && reportBuffer[0] == 0x01) {
            io4_report_in* inRep = (io4_report_in*)reportBuffer;

            // Use JOYSTICK_POSITION as defined in your public.h
            JOYSTICK_POSITION iReport;
            memset(&iReport, 0, sizeof(iReport));
            iReport.bDevice = (BYTE)DevID;

            // Map analog axes based on your JOYSTICK_POSITION_V3 structure
            // Adjust these mappings based on how your vJoy device is configured
            iReport.wAxisX = inRep->adcs[0];
            iReport.wAxisY = inRep->adcs[1];
            iReport.wAxisZ = inRep->adcs[2];
            iReport.wRudder = inRep->spinners[0];     // Example mapping
            iReport.wAileron = inRep->spinners[1];    // Example mapping
            // Add more axis mappings as needed

            // Map buttons (assuming buttons[0] and buttons[1] contain 16 buttons each)
            iReport.lButtons = 0;
            iReport.lButtons |= (inRep->buttons[0] & 0xFFFF);        // Buttons 1-16
            iReport.lButtons |= ((inRep->buttons[1] & 0xFFFF) << 16); // Buttons 17-32

            // Update vJoy with the new state
            if (!UpdateVJD(DevID, &iReport)) {
                printf("Feeding vJoy device failed. Trying to re-acquire.\n");
                if (!AcquireVJD(DevID)) {
                    printf("Failed to re-acquire vJoy.\n");
                    break;
                }
            }
        }

        Sleep(5); // Small delay to prevent busy-looping
    }

    // Cleanup
    CloseHandle(io4Handle);
    RelinquishVJD(DevID);
    return 0;
}
