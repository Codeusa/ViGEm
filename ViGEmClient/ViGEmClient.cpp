/*
MIT License

Copyright (c) 2017 Benjamin "Nefarius" H�glinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdlib.h>
#include <SetupAPI.h>
#include <initguid.h>

#include "ViGEmClient.h"
#include <winioctl.h>
#include <limits.h>
#include <mutex>

#define VIGEM_TARGETS_MAX   USHRT_MAX

typedef struct _VIGEM_CLIENT_T
{
    HANDLE hBusDevice;

} VIGEM_CLIENT_T;

typedef enum _VIGEM_TARGET_STATE
{
    VIGEM_TARGET_NEW,
    VIGEM_TARGET_INITIALIZED,
    VIGEM_TARGET_CONNECTED,
    VIGEM_TARGET_DISCONNECTED
} VIGEM_TARGET_STATE, *PVIGEM_TARGET_STATE;

//
// Represents a virtual gamepad object.
// 
typedef struct _VIGEM_TARGET_T
{
    ULONG Size;
    ULONG SerialNo;
    VIGEM_TARGET_STATE State;
    USHORT VendorId;
    USHORT ProductId;
    VIGEM_TARGET_TYPE Type;
    DWORD_PTR Notification;

} VIGEM_TARGET;

//
// Initializes a virtual gamepad object.
// 
PVIGEM_TARGET FORCEINLINE VIGEM_TARGET_ALLOC_INIT(
    _In_ VIGEM_TARGET_TYPE Type
)
{
    auto target = static_cast<PVIGEM_TARGET>(malloc(sizeof(VIGEM_TARGET)));
    RtlZeroMemory(target, sizeof(VIGEM_TARGET));

    target->Size = sizeof(VIGEM_TARGET);
    target->State = VIGEM_TARGET_INITIALIZED;
    target->Type = Type;

    return target;
}


PVIGEM_CLIENT vigem_alloc()
{
    auto driver = static_cast<PVIGEM_CLIENT>(malloc(sizeof(VIGEM_CLIENT_T)));
    
    RtlZeroMemory(driver, sizeof(VIGEM_CLIENT_T));
    driver->hBusDevice = INVALID_HANDLE_VALUE;

    return driver;
}

void vigem_free(PVIGEM_CLIENT vigem)
{
    if (vigem)
        free(vigem);
}

VIGEM_ERROR vigem_connect(PVIGEM_CLIENT vigem)
{
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData = { 0 };
    deviceInterfaceData.cbSize = sizeof(deviceInterfaceData);
    DWORD memberIndex = 0;
    DWORD requiredSize = 0;
    auto error = VIGEM_ERROR_BUS_NOT_FOUND;

    // check for already open handle as re-opening accidentally would destroy all live targets
    if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
    {
        return VIGEM_ERROR_BUS_ALREADY_CONNECTED;
    }

    auto deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_BUSENUM_VIGEM, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    // enumerate device instances
    while (SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM, memberIndex++, &deviceInterfaceData))
    {
        // get required target buffer size
        SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, nullptr, 0, &requiredSize, nullptr);

        // allocate target buffer
        auto detailDataBuffer = static_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(malloc(requiredSize));
        detailDataBuffer->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        // get detail buffer
        if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, detailDataBuffer, requiredSize, &requiredSize, nullptr))
        {
            SetupDiDestroyDeviceInfoList(deviceInfoSet);
            free(detailDataBuffer);
            error = VIGEM_ERROR_BUS_NOT_FOUND;
            continue;
        }

        // bus found, open it
        vigem->hBusDevice = CreateFile(detailDataBuffer->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);

        // check bus open result
        if (vigem->hBusDevice == INVALID_HANDLE_VALUE)
        {
            error = VIGEM_ERROR_BUS_ACCESS_FAILED;
            free(detailDataBuffer);
            continue;
        }

        DWORD transfered = 0;
        OVERLAPPED lOverlapped = { 0 };
        lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        VIGEM_CHECK_VERSION version;
        VIGEM_CHECK_VERSION_INIT(&version, VIGEM_COMMON_VERSION);

        // send compiled library version to driver to check compatibility
        DeviceIoControl(vigem->hBusDevice, IOCTL_VIGEM_CHECK_VERSION, &version, version.Size, nullptr, 0, &transfered, &lOverlapped);

        // wait for result
        if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transfered, TRUE) != 0)
        {
            error = VIGEM_ERROR_NONE;
            free(detailDataBuffer);
            CloseHandle(lOverlapped.hEvent);
            break;
        }

        error = VIGEM_ERROR_BUS_VERSION_MISMATCH;

        CloseHandle(lOverlapped.hEvent);
        free(detailDataBuffer);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    return error;
}

void vigem_disconnect(PVIGEM_CLIENT vigem)
{
    if (vigem->hBusDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(vigem->hBusDevice);
        vigem->hBusDevice = INVALID_HANDLE_VALUE;
    }
}

PVIGEM_TARGET vigem_target_x360_alloc(void)
{
    return VIGEM_TARGET_ALLOC_INIT(Xbox360Wired);
}

PVIGEM_TARGET vigem_target_ds4_alloc(void)
{
    return VIGEM_TARGET_ALLOC_INIT(DualShock4Wired);
}

void vigem_target_free(PVIGEM_TARGET target)
{
    if (target)
        free(target);
}

VIGEM_ERROR vigem_target_add(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->State == VIGEM_TARGET_NEW)
    {
        return VIGEM_ERROR_TARGET_UNINITIALIZED;
    }

    if (target->State == VIGEM_TARGET_CONNECTED)
    {
        return VIGEM_ERROR_ALREADY_CONNECTED;
    }

    DWORD transfered = 0;
    VIGEM_PLUGIN_TARGET plugin;
    OVERLAPPED lOverlapped = { 0 };
    lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    for (target->SerialNo = 1; target->SerialNo <= VIGEM_TARGETS_MAX; target->SerialNo++)
    {
        VIGEM_PLUGIN_TARGET_INIT(&plugin, target->SerialNo, target->Type);

        plugin.VendorId = target->VendorId;
        plugin.ProductId = target->ProductId;

        DeviceIoControl(vigem->hBusDevice, IOCTL_VIGEM_PLUGIN_TARGET, &plugin, plugin.Size, nullptr, 0, &transfered, &lOverlapped);

        if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transfered, TRUE) != 0)
        {
            target->State = VIGEM_TARGET_CONNECTED;
            CloseHandle(lOverlapped.hEvent);

            return VIGEM_ERROR_NONE;
        }
    }

    CloseHandle(lOverlapped.hEvent);

    return VIGEM_ERROR_NO_FREE_SLOT;
}

VIGEM_ERROR vigem_target_remove(PVIGEM_CLIENT vigem, PVIGEM_TARGET target)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->State == VIGEM_TARGET_NEW)
    {
        return VIGEM_ERROR_TARGET_UNINITIALIZED;
    }

    if (target->State != VIGEM_TARGET_CONNECTED)
    {
        return VIGEM_ERROR_TARGET_NOT_PLUGGED_IN;
    }

    DWORD transfered = 0;
    VIGEM_UNPLUG_TARGET unplug;
    OVERLAPPED lOverlapped = { 0 };
    lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    VIGEM_UNPLUG_TARGET_INIT(&unplug, target->SerialNo);

    DeviceIoControl(vigem->hBusDevice, IOCTL_VIGEM_UNPLUG_TARGET, &unplug, unplug.Size, nullptr, 0, &transfered, &lOverlapped);

    if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transfered, TRUE) != 0)
    {
        target->State = VIGEM_TARGET_DISCONNECTED;
        CloseHandle(lOverlapped.hEvent);

        //g_xusbCallbacks.erase(Target->SerialNo);
        //g_ds4Callbacks.erase(Target->SerialNo);

        return VIGEM_ERROR_NONE;
    }

    CloseHandle(lOverlapped.hEvent);

    return VIGEM_ERROR_REMOVAL_FAILED;
}

VIGEM_ERROR vigem_target_x360_register_notification(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, PVIGEM_X360_NOTIFICATION notification)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->SerialNo == 0 || notification == nullptr)
    {
        return VIGEM_ERROR_INVALID_TARGET;
    }

    if (target->Notification == reinterpret_cast<DWORD_PTR>(notification))
    {
        return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;
    }

    target->Notification = reinterpret_cast<DWORD_PTR>(notification);

    std::thread _async{ [](
        PVIGEM_TARGET _Target,
        PVIGEM_CLIENT _Client)
    {
        DWORD error = ERROR_SUCCESS;
        DWORD transfered = 0;
        OVERLAPPED lOverlapped = { 0 };
        lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        XUSB_REQUEST_NOTIFICATION notify;
        XUSB_REQUEST_NOTIFICATION_INIT(&notify, _Target->SerialNo);

        do
        {
            DeviceIoControl(_Client->hBusDevice, IOCTL_XUSB_REQUEST_NOTIFICATION, &notify, notify.Size, &notify, notify.Size, &transfered, &lOverlapped);

            if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped, &transfered, TRUE) != 0)
            {
                if (_Target->Notification == NULL)
                {
                    CloseHandle(lOverlapped.hEvent);
                    return;
                }

                reinterpret_cast<PVIGEM_X360_NOTIFICATION>(_Target->Notification)(_Target->SerialNo, notify.LargeMotor, notify.SmallMotor, notify.LedNumber);
            }
            else
            {
                error = GetLastError();
            }
        } while (error != ERROR_OPERATION_ABORTED && error != ERROR_ACCESS_DENIED);

        CloseHandle(lOverlapped.hEvent);

    }, target, vigem };

    _async.detach();

    return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_register_notification(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, PVIGEM_DS4_NOTIFICATION notification)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->SerialNo == 0 || notification == nullptr)
    {
        return VIGEM_ERROR_INVALID_TARGET;
    }

    if (target->Notification == reinterpret_cast<DWORD_PTR>(notification))
    {
        return VIGEM_ERROR_CALLBACK_ALREADY_REGISTERED;
    }

    target->Notification = reinterpret_cast<DWORD_PTR>(notification);

    std::thread _async{ [](
        PVIGEM_TARGET _Target,
        PVIGEM_CLIENT _Client)
    {
        DWORD error = ERROR_SUCCESS;
        DWORD transfered = 0;
        OVERLAPPED lOverlapped = { 0 };
        lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        DS4_REQUEST_NOTIFICATION notify;
        DS4_REQUEST_NOTIFICATION_INIT(&notify, _Target->SerialNo);

        do
        {
            DeviceIoControl(_Client->hBusDevice, IOCTL_DS4_REQUEST_NOTIFICATION, &notify, notify.Size, &notify, notify.Size, &transfered, &lOverlapped);

            if (GetOverlappedResult(_Client->hBusDevice, &lOverlapped, &transfered, TRUE) != 0)
            {
                if (_Target->Notification == NULL)
                {
                    CloseHandle(lOverlapped.hEvent);
                    return;
                }

                reinterpret_cast<PVIGEM_DS4_NOTIFICATION>(_Target->Notification)(_Target->SerialNo, notify.Report.LargeMotor, notify.Report.SmallMotor, notify.Report.LightbarColor);
            }
            else
            {
                error = GetLastError();
            }
        } while (error != ERROR_OPERATION_ABORTED && error != ERROR_ACCESS_DENIED);

        CloseHandle(lOverlapped.hEvent);

    }, target, vigem };

    _async.detach();

    return VIGEM_ERROR_NONE;
}

void vigem_target_x360_unregister_notification(PVIGEM_TARGET target)
{
    target->Notification = NULL;
}

void vigem_target_ds4_unregister_notification(PVIGEM_TARGET target)
{
    target->Notification = NULL;
}

void vigem_target_set_vid(PVIGEM_TARGET target, USHORT vid)
{
    target->VendorId = vid;
}

void vigem_target_set_pid(PVIGEM_TARGET target, USHORT pid)
{
    target->ProductId = pid;
}

VIGEM_ERROR vigem_target_x360_update(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, XUSB_REPORT report)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->SerialNo == 0)
    {
        return VIGEM_ERROR_INVALID_TARGET;
    }

    DWORD transfered = 0;
    OVERLAPPED lOverlapped = { 0 };
    lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    XUSB_SUBMIT_REPORT xsr;
    XUSB_SUBMIT_REPORT_INIT(&xsr, target->SerialNo);

    xsr.Report = report;

    DeviceIoControl(vigem->hBusDevice, IOCTL_XUSB_SUBMIT_REPORT, &xsr, xsr.Size, nullptr, 0, &transfered, &lOverlapped);

    if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transfered, TRUE) == 0)
    {
        if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            CloseHandle(lOverlapped.hEvent);
            return VIGEM_ERROR_INVALID_TARGET;
        }
    }

    CloseHandle(lOverlapped.hEvent);

    return VIGEM_ERROR_NONE;
}

VIGEM_ERROR vigem_target_ds4_update(PVIGEM_CLIENT vigem, PVIGEM_TARGET target, DS4_REPORT report)
{
    if (vigem->hBusDevice == nullptr)
    {
        return VIGEM_ERROR_BUS_NOT_FOUND;
    }

    if (target->SerialNo == 0)
    {
        return VIGEM_ERROR_INVALID_TARGET;
    }

    DWORD transfered = 0;
    OVERLAPPED lOverlapped = { 0 };
    lOverlapped.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    DS4_SUBMIT_REPORT dsr;
    DS4_SUBMIT_REPORT_INIT(&dsr, target->SerialNo);

    dsr.Report = report;

    DeviceIoControl(vigem->hBusDevice, IOCTL_DS4_SUBMIT_REPORT, &dsr, dsr.Size, nullptr, 0, &transfered, &lOverlapped);

    if (GetOverlappedResult(vigem->hBusDevice, &lOverlapped, &transfered, TRUE) == 0)
    {
        if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            CloseHandle(lOverlapped.hEvent);
            return VIGEM_ERROR_INVALID_TARGET;
        }
    }

    CloseHandle(lOverlapped.hEvent);

    return VIGEM_ERROR_NONE;
}

ULONG vigem_target_get_index(PVIGEM_TARGET target)
{
    return target->SerialNo;
}
