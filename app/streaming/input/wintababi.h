#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

// WinTab 1.4 ABI subset. Definitions follow the Wacom SDK WINTAB.H:
// https://github.com/Wacom-Developer/wacom-device-kit-windows/blob/master/WintabSDK/WINTAB.H
// Copyright Wacom Company, Ltd. 2024, portions 1991-1998 LCS/Telegraphics.
// The header permits its text and information to be freely used, copied and
// distributed without compensation or licensing restrictions.
namespace WinTabAbi {
struct Axis { LONG minimum, maximum; UINT units; DWORD resolution; };
struct Orientation { int azimuth, altitude, twist; };
struct Context {
    char name[40];
    UINT options, status, locks, messageBase, device, packetRate;
    DWORD packetData, packetMode, moveMask, buttonDownMask, buttonUpMask;
    LONG inputX, inputY, inputZ, inputWidth, inputHeight, inputDepth;
    LONG outputX, outputY, outputZ, outputWidth, outputHeight, outputDepth;
    DWORD sensitivityX, sensitivityY, sensitivityZ;
    BOOL systemMode;
    int systemX, systemY, systemWidth, systemHeight;
    DWORD systemSensitivityX, systemSensitivityY;
};
struct Packet {
    UINT status, cursor;
    DWORD buttons;
    LONG x, y;
    UINT pressure;
    Orientation orientation;
};
static_assert(sizeof(Context) == 172, "WinTab LOGCONTEXTA layout must match driver ABI");
static_assert(sizeof(Packet) == 36, "WinTab packet layout must match requested packet mask");
constexpr UINT interfaceCategory = 1, defaultSystemContext = 4, deviceCategory = 100, cursorCategory = 200;
constexpr UINT deviceCount = 4, deviceNormalPressure = 15, deviceOrientation = 17;
constexpr UINT cursorPacketData = 3, cursorButtonCount = 4, cursorButtonMap = 7, cursorNormalPressureButton = 9;
constexpr DWORD packetStatus = 0x2, packetCursor = 0x20, packetButtons = 0x40, packetX = 0x80, packetY = 0x100, packetPressure = 0x400, packetOrientation = 0x1000;
constexpr UINT systemCursor = 0x1, messages = 0x4, proximityStatus = 0x1, queueErrorStatus = 0x2, invertedStatus = 0x10;
using Info = UINT (APIENTRY *)(UINT, UINT, void*);
using Open = HANDLE (APIENTRY *)(HWND, Context*, BOOL);
using Close = BOOL (APIENTRY *)(HANDLE);
using GetPacket = BOOL (APIENTRY *)(HANDLE, UINT, void*);
using Enable = BOOL (APIENTRY *)(HANDLE, BOOL);
using Overlap = BOOL (APIENTRY *)(HANDLE, BOOL);
}
