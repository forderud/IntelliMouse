#pragma once
#include <windows.h>
#include <cfgmgr32.h> // for CM_Get_Device_Interface_List
#include <Hidclass.h> // for GUID_DEVINTERFACE_HID
#include <hidsdi.h>
#include <wrl/wrappers/corewrappers.h> // for FileHandle RAII wrapper

#include <cassert>
#include <string>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "mincore.lib")

namespace hid {

/** RAII wrapper of PHIDP_PREPARSED_DATA. */
class PreparsedData {
public:
    PreparsedData() = default;

    PreparsedData(PreparsedData&& obj) noexcept {
        std::swap(report, obj.report); // avoid double delete
    }

    ~PreparsedData() {
        Close();
    }

    BOOLEAN Open(HANDLE hid_dev) {
        assert(!report);
        return HidD_GetPreparsedData(hid_dev, &report);
    }
    void Close() {
        if (report) {
            HidD_FreePreparsedData(report);
            report = nullptr;
        }
    }

    operator PHIDP_PREPARSED_DATA() const {
        return report;
    }

private:
    PHIDP_PREPARSED_DATA report = nullptr; // report descriptor for top-level collection
};

/** Win32 HID device wrapper class.
    Simplifies accss to INPUT, OUTPUT and FEATURE reports, as well as device strings. */
class Device {
public:
    Device() = default;

    Device(const wchar_t* deviceName) : devName(deviceName) {
        dev.Attach(CreateFileW(deviceName,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL));
        if (!dev.IsValid()) {
            DWORD err = GetLastError(); err;
            //assert(err != ERROR_ACCESS_DENIED); // (5) observed for already used devices
            //assert(err != ERROR_SHARING_VIOLATION); // (32)
            //wprintf(L"WARNING: CreateFile failed: (err %d) for %ls\n", err, deviceName);
            return;
        }

        if (!HidD_GetAttributes(dev.Get(), &attr)) {
            DWORD err = GetLastError(); err;
            assert(err == ERROR_NOT_FOUND);
            dev.Close(); // invalidate object
            return;
        }

        if (!preparsed.Open(dev.Get())) {
            // Have never encountered this problem
            dev.Close(); // invalidate object
            return;
        }

        if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) {
            // Have never encountered this problem
            dev.Close(); // invalidate object
            return;
        }

#ifndef NDEBUG
        Manufacturer = GetManufacturer();
        Product = GetProduct();
        SerialNumber = GetSerialNumber();
#endif
    }

    bool IsValid() const {
        return dev.IsValid();
    }

    std::wstring GetManufacturer() const {
        wchar_t man_buffer[128] = L""; // max USB length is 126 wchar's
        HidD_GetManufacturerString(dev.Get(), man_buffer, (ULONG)std::size(man_buffer)); // ignore errors
        return man_buffer;
    }

    std::wstring GetProduct() const {
        wchar_t prod_buffer[128] = L""; // max USB length is 126 wchar's
        HidD_GetProductString(dev.Get(), prod_buffer, (ULONG)std::size(prod_buffer)); // ignore erorrs
        return prod_buffer;
    }

    std::wstring GetSerialNumber() const {
        wchar_t sn_buffer[128] = L""; // max USB length is 126 wchar's
        HidD_GetSerialNumberString(dev.Get(), sn_buffer, (ULONG)std::size(sn_buffer)); // ignore erorrs
        return sn_buffer;
    }

    std::wstring GetString(ULONG idx) const {
        wchar_t buffer[128] = L""; // max USB length is 126 wchar's
        BOOLEAN ok = HidD_GetIndexedString(dev.Get(), idx, buffer, (ULONG)std::size(buffer));
        assert(ok); ok;
        return buffer;
    }

    /** Get typed FEATURE or INPUT report with ReportID prefix. */
    template <class T>
    T GetReport(HIDP_REPORT_TYPE type) const {
        T report{}; // assume report ID prefix on first byte

        BOOLEAN ok = false;
        if (type == HidP_Input) {
            assert(sizeof(report) == caps.InputReportByteLength);
            ok = HidD_GetInputReport(dev.Get(), &report, sizeof(report));
        }  else if (type == HidP_Feature) {
            assert(sizeof(report) == caps.FeatureReportByteLength);
            ok = HidD_GetFeature(dev.Get(), &report, sizeof(report));
        }
        if (!ok) {
            DWORD err = GetLastError();
            wprintf(L"ERROR: HidD_GetFeature or HidD_GetInputReport failure (err %d).\n", err);
            assert(ok);
            return {};
        }

        return report;
    }

    /** Get FEATURE or INPUT report as byte array without ReportID prefix. */
    std::vector<BYTE> GetReport(HIDP_REPORT_TYPE type, BYTE reportId) const {
        std::vector<BYTE> report(1, (BYTE)0);
        report[0] = reportId; // report ID prefix

        BOOLEAN ok = false;
        if (type == HidP_Input) {
            report.resize(caps.InputReportByteLength, (BYTE)0);
            ok = HidD_GetInputReport(dev.Get(), report.data(), (ULONG)report.size());
        } else if (type == HidP_Feature) {
            report.resize(caps.FeatureReportByteLength, (BYTE)0);
            ok = HidD_GetFeature(dev.Get(), report.data(), (ULONG)report.size());
        }
        if (!ok) {
            DWORD err = GetLastError();
            wprintf(L"ERROR: HidD_GetInputReport failure (err %d).\n", err);
            assert(ok);
            return {};
        }

        // remove report ID prefix
        report.erase(report.begin());

        return report;
    }

    /** Set FEATURE or OUTPUT report with ReportID prefix. */
    template <class T>
    bool SetReport(HIDP_REPORT_TYPE type, const T& report) {
        BOOLEAN ok = false;
        if (type == HidP_Output) {
            assert(sizeof(report) == caps.OutputReportByteLength);
            ok = HidD_SetOutputReport(dev.Get(), const_cast<void*>(static_cast<const void*>(&report)), sizeof(report));
        } else if (type == HidP_Feature) {
            assert(sizeof(report) == caps.FeatureReportByteLength);
            ok = HidD_SetFeature(dev.Get(), const_cast<void*>(static_cast<const void*>(&report)), sizeof(report));
        }
        if (!ok) {
            DWORD err = GetLastError();
            wprintf(L"ERROR: HidD_SetFeature failure (err %d).\n", err);
            assert(ok);
            return {};
        }

        return ok;
    }

    std::vector<HIDP_VALUE_CAPS> GetValueCaps(HIDP_REPORT_TYPE type) const {
        USHORT valueCapsLen = 0;
        if (type == HidP_Input)
            valueCapsLen = caps.NumberInputValueCaps;
        else if (type == HidP_Output)
            valueCapsLen = caps.NumberOutputValueCaps;
        else if (type == HidP_Feature)
            valueCapsLen = caps.NumberFeatureValueCaps;
        if (valueCapsLen == 0)
            return {};

        std::vector<HIDP_VALUE_CAPS> valueCaps(valueCapsLen, HIDP_VALUE_CAPS{});
        NTSTATUS status = HidP_GetValueCaps(type, valueCaps.data(), &valueCapsLen, preparsed);
        if (status == HIDP_STATUS_INVALID_PREPARSED_DATA) {
            wprintf(L"WARNING: Invalid preparsed data.\n");
            return {};
        }

        assert(status == HIDP_STATUS_SUCCESS); status;
        return valueCaps;
    }

    void PrintInfo() const {
        wprintf(L"Device %ls (VendorID=%x, ProductID=%x, Usage=%x, UsagePage=%x)\n", devName.c_str(), attr.VendorID, attr.ProductID, caps.Usage, caps.UsagePage);

        wprintf(L"Device capabilities:\n");
        wprintf(L"  Usage=0x%04X, UsagePage=0x%04X\n", caps.Usage, caps.UsagePage);
        wprintf(L"  InputReportByteLength=%u, OutputReportByteLength=%u, FeatureReportByteLength=%u, NumberLinkCollectionNodes=%u\n", caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength, caps.NumberLinkCollectionNodes);
        wprintf(L"  NumberInputButtonCaps=%u, NumberInputValueCaps=%u, NumberInputDataIndices=%u\n", caps.NumberInputButtonCaps, caps.NumberInputValueCaps, caps.NumberInputDataIndices);
        wprintf(L"  NumberOutputButtonCaps=%u, NumberOutputValueCaps=%u, NumberOutputDataIndices=%u\n", caps.NumberOutputButtonCaps, caps.NumberOutputValueCaps, caps.NumberOutputDataIndices);
        wprintf(L"  NumberFeatureButtonCaps=%u, NumberFeatureValueCaps=%u, NumberFeatureDataIndices=%u\n", caps.NumberFeatureButtonCaps, caps.NumberFeatureValueCaps, caps.NumberFeatureDataIndices);
    }

public:
    std::wstring devName;
private:
    Microsoft::WRL::Wrappers::FileHandle dev;
public:
    HIDD_ATTRIBUTES attr = {}; // VendorID, ProductID, VersionNumber
private:
    PreparsedData preparsed; // opaque ptr
public:
    HIDP_CAPS caps = {}; // Usage, UsagePage, report sizes

#ifndef NDEBUG
    // fields to aid debugging
    std::wstring Manufacturer;
    std::wstring Product;
    std::wstring SerialNumber;
#endif
};

/** Human Interface Devices (HID) device search class. */
class Scan {
public:
    struct Criterion {
        USHORT VendorID = 0;
        USHORT ProductID = 0;
        USHORT Usage = 0;
        USHORT UsagePage = 0;
    };

    static std::vector<Device> FindDevices (const Criterion& crit) {
        const ULONG searchScope = CM_GET_DEVICE_INTERFACE_LIST_PRESENT; // only currently 'live' device interfaces

        ULONG deviceInterfaceListLength = 0;
        CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&deviceInterfaceListLength, (GUID*)&GUID_DEVINTERFACE_HID, NULL, searchScope);
        assert(cr == CR_SUCCESS);

        // symbolic link name of interface instances
        std::wstring deviceInterfaceList(deviceInterfaceListLength, L'\0');
        cr = CM_Get_Device_Interface_ListW((GUID*)&GUID_DEVINTERFACE_HID, NULL, deviceInterfaceList.data(), deviceInterfaceListLength, searchScope);
        assert(cr == CR_SUCCESS);

        std::vector<Device> results;
        for (const wchar_t * currentInterface = deviceInterfaceList.c_str(); *currentInterface; currentInterface += wcslen(currentInterface) + 1) {
            Device dev(currentInterface);

            if (CheckDevice(dev, crit))
                results.push_back(std::move(dev));
        }

        return results;
    }

private:
    static bool CheckDevice(Device & dev, const Criterion& crit) {
        if (!dev.IsValid())
            return false;

        if (crit.VendorID && (crit.VendorID != dev.attr.VendorID))
            return false;
        if (crit.ProductID && (crit.ProductID != dev.attr.ProductID))
            return false;

        if (crit.Usage && (crit.Usage != dev.caps.Usage))
            return false;
        if (crit.UsagePage && (crit.UsagePage != dev.caps.UsagePage))
            return false;

        // found matching device
        return true;
    }
};

} // namespace hid
