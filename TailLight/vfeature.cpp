#include "driver.h"
#include <hidpddi.h>
#include <hidclass.h>
#include "eventlog.h"
#include "CppAllocator.hpp"
#include "inc\TailLight.h"

/** RAII wrapper of PHIDP_PREPARSED_DATA. */
class PHIDP_PREPARSED_DATA_Wrap {
public:
    PHIDP_PREPARSED_DATA_Wrap(size_t size) {
        m_ptr = new BYTE[size];
    }
    ~PHIDP_PREPARSED_DATA_Wrap() {
        if (m_ptr) {
            delete[] m_ptr;
            m_ptr = nullptr;
        }
    }

    operator PHIDP_PREPARSED_DATA () const {
        return (PHIDP_PREPARSED_DATA)m_ptr;
    }

private:
    BYTE* m_ptr = nullptr;
};

/** RAII wrapper of WDFIOTARGET. */
class WDFIOTARGET_Wrap {
public:
    WDFIOTARGET_Wrap() {
    }
    ~WDFIOTARGET_Wrap() {
        if (m_obj != NULL) {
            WdfObjectDelete(m_obj);
        }
    }

    operator WDFIOTARGET () const {
        return m_obj;
    }
    WDFIOTARGET* operator & () {
        return &m_obj;
    }

private:
    WDFIOTARGET m_obj = NULL;
};


NTSTATUS SetFeatureColor (
    _In_ WDFDEVICE Device,
    _In_ ULONG     Color
    )
/*++
    This routine sets the HID feature by sending HID ioctls to our device.
    These IOCTLs will be handled by HIDUSB and converted into USB requests
    and send to the device.
--*/
{
    KdPrint(("TailLight: SetFeatureColor\n"));

    WDFIOTARGET_Wrap hidTarget;
    {
        // open "hidTarget" using PdoName
        NTSTATUS status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &hidTarget);
        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetCreate failed 0x%x\n", status));
            return status;
        }

        // open in write-only mode
        DEVICE_CONTEXT* deviceContext = WdfObjectGet_DEVICE_CONTEXT(Device);
        WDF_IO_TARGET_OPEN_PARAMS openParams = {};
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &deviceContext->PdoName, FILE_WRITE_ACCESS);

        // We will let the framework to respond automatically to the pnp
        // state changes of the target by closing and opening the handle.
        openParams.ShareAccess = FILE_SHARE_WRITE | FILE_SHARE_READ;

        status = WdfIoTargetOpen(hidTarget, &openParams);
        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetOpen failed 0x%x\n", status));
            return status;
        }
    }

    HID_COLLECTION_INFORMATION collectionInfo = {};
    {
        // populate "collectionInformation"
        WDF_MEMORY_DESCRIPTOR collectionInfoDesc = {};
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&collectionInfoDesc, &collectionInfo, sizeof(HID_COLLECTION_INFORMATION));

        NTSTATUS status = WdfIoTargetSendIoctlSynchronously(hidTarget,
            NULL,
            IOCTL_HID_GET_COLLECTION_INFORMATION,
            NULL,
            &collectionInfoDesc,
            NULL,
            NULL);

        KdPrint(("TailLight: ProductID=%x, VendorID=%x, VersionNumber=%u, DescriptorSize=%u\n", collectionInfo.ProductID, collectionInfo.VendorID, collectionInfo.VersionNumber, collectionInfo.DescriptorSize));

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetSendIoctlSynchronously1 failed 0x%x\n", status));
            return status;
        }
    }

    PHIDP_PREPARSED_DATA_Wrap preparsedData(collectionInfo.DescriptorSize);
    if (!preparsedData) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        // populate "preparsedData"
        WDF_MEMORY_DESCRIPTOR preparsedDataDesc = {};
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&preparsedDataDesc, static_cast<PHIDP_PREPARSED_DATA>(preparsedData), collectionInfo.DescriptorSize);

        NTSTATUS status = WdfIoTargetSendIoctlSynchronously(hidTarget,
            NULL,
            IOCTL_HID_GET_COLLECTION_DESCRIPTOR, // same as HidD_GetPreparsedData in user-mode
            NULL,
            &preparsedDataDesc,
            NULL,
            NULL);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetSendIoctlSynchronously2 failed 0x%x\n", status));
            return status;
        }
    }

    {
        // get capabilities
        HIDP_CAPS caps = {};
        NTSTATUS status = HidP_GetCaps(preparsedData, &caps);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        //KdPrint(("TailLight: Usage=%x, UsagePage=%x\n", caps.Usage, caps.UsagePage));

        if (caps.FeatureReportByteLength != sizeof(TailLightReport)) {
            KdPrint(("TailLight: FeatureReportByteLength mismatch (%u, %Iu).\n", caps.FeatureReportByteLength, sizeof(TailLightReport)));
            return status;
        }
    }

    // Create a report to send to the device.
    TailLightReport report;
    SetColor(&report, Color);

    {
        // send TailLightReport to device
        WDF_MEMORY_DESCRIPTOR reportDesc = {};
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&reportDesc, &report, sizeof(report));
        NTSTATUS status = WdfIoTargetSendIoctlSynchronously(hidTarget,
            NULL,
            IOCTL_HID_SET_FEATURE, // 0xb0191
            &reportDesc,
            NULL,
            NULL,
            NULL);
        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoTargetSendIoctlSynchronously3 failed 0x%x\n", status));
            return status;
        }
    }

    return STATUS_SUCCESS;
}


TailLightReport* SetFeatureFilter(
    _In_ WDFDEVICE  Device,
    _In_ WDFREQUEST Request,
    _In_ size_t     InputBufferLength

)
/*++
Routine Description:
    Handles IOCTL_HID_SET_FEATURE for all the collection.
    For control collection (custom defined collection) it handles
    the user-defined control codes for sideband communication

Arguments:
    QueueContext - The object context associated with the queue

    Request - Pointer to Request Packet.
--*/
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    size_t finalLength = 0;
    TailLightReport* pReport = nullptr;

    KdPrint(("TailLight: SetFeatureFilter\n"));
    DEVICE_CONTEXT* deviceContext = WdfObjectGet_DEVICE_CONTEXT(Device);

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(TailLightReport), (void**)&pReport, &finalLength);

    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfRequestRetrieveInputBuffer failed 0x%x, packet=0x%p final length0x%zx\n", status, pReport, finalLength));
        return nullptr;
    }

    if (InputBufferLength != sizeof(TailLightReport)) {
        KdPrint(("TailLight: SetFeatureFilter: Incorrect InputBufferLength\n"));
        return nullptr;
    }

    if (!IsValid(pReport)) {
        // If collection ID is not for control collection then handle
        // this request just as you would for a regular collection.
        return nullptr;
    }

    // capture color before safety adjustments
    UCHAR r = pReport->Red;
    UCHAR g = pReport->Green;
    UCHAR b = pReport->Blue;
    
    // Enforce safety limits (sets color to RED on failure)
    if (!SafetyCheck(pReport)) {
        // log safety violation to Windows Event Viewer "System" log
        WCHAR color_requested[16] = {};
        swprintf_s(color_requested, L"%u,%u,%u", r, g, b);
        WCHAR color_adjusted[16] = {};
        swprintf_s(color_adjusted, L"%u,%u,%u", pReport->Red, pReport->Green, pReport->Blue);

        WriteToSystemLog(Device, TailLight_SAFETY, color_requested, color_adjusted);
        KdPrint(("TailLight: Color safety check applied\n"));
    }

    SetColor(pReport, 0x00FF00);
    // update last written color
    deviceContext->TailLight = GetColor(pReport);

    KdPrint(("TailLight: Sending color request down.\n"));

    return pReport;
}
