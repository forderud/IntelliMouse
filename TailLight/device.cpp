#include "driver.h"
#include <Hidport.h>

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControlFilter;


NTSTATUS EvtDriverDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
/*++
Routine Description:
    EvtDriverDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent to be part of the device stack as a filter.

Arguments:
    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.
--*/    
{
    UNREFERENCED_PARAMETER(Driver);

    // Configure the device as a filter driver
    WdfFdoInitSetFilter(DeviceInit);

    WDFDEVICE device = 0;
    {
        // create device
        WDF_OBJECT_ATTRIBUTES attributes = {};
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

        NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfDeviceCreate, Error %x\n", status));
            return status;
        }
    }

    // Driver Framework always zero initializes an objects context memory
    DEVICE_CONTEXT* deviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

    {
        // initialize DEVICE_CONTEXT struct with PdoName

        // In order to send ioctls to our PDO, we have open to open it
        // by name so that we have a valid filehandle (fileobject).
        // When we send ioctls using the IoTarget, framework automatically 
        // sets the filobject in the stack location.
        WDF_OBJECT_ATTRIBUTES attributes = {};
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device; // auto-delete with device

        WDFMEMORY memory = 0;
        NTSTATUS status = WdfDeviceAllocAndQueryProperty(device,
            DevicePropertyPhysicalDeviceObjectName,
            NonPagedPoolNx,
            &attributes,
            &memory);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfDeviceAllocAndQueryProperty failed 0x%x\n", status));
            return STATUS_UNSUCCESSFUL;
        }

        // initialize pDeviceContext->PdoName based on memory
        size_t bufferLength = 0;
        deviceContext->PdoName.Buffer = (WCHAR*)WdfMemoryGetBuffer(memory, &bufferLength);
        if (deviceContext->PdoName.Buffer == NULL)
            return STATUS_UNSUCCESSFUL;

        deviceContext->PdoName.MaximumLength = (USHORT)bufferLength;
        deviceContext->PdoName.Length = (USHORT)bufferLength - sizeof(UNICODE_NULL);

        KdPrint(("TailLight: PdoName: %wZ\n", deviceContext->PdoName)); // outputs "\Device\00000083
    }

    {
        // create queue to process IOCTLs
        WDF_IO_QUEUE_CONFIG queueConfig = {};
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel); // don't synchronize
        //queueConfig.EvtIoRead // pass-through read requests 
        //queueConfig.EvtIoWrite // pass-through write requests
        queueConfig.EvtIoDeviceControl = EvtIoDeviceControlFilter; // filter I/O device control requests

        WDFQUEUE queue = 0; // auto-deleted when parent is deleted
        NTSTATUS status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);

        if (!NT_SUCCESS(status)) {
            KdPrint(("TailLight: WdfIoQueueCreate failed 0x%x\n", status));
            return status;
        }
    }

    // Initialize WMI provider
    NTSTATUS status = WmiInitialize(device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: Error initializing WMI 0x%x\n", status));
        return status;
    }

    status = TailLight_SetBlack(deviceContext);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: Error setting taillight to black 0x%x\n", status));
        return status;
    }

    return status;
}

NTSTATUS TailLight_SetBlack(DEVICE_CONTEXT* pDeviceContext)
{
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    PCHAR               report = NULL;
    NTSTATUS            status;
    WDFIOTARGET         hidTarget = NULL;

    PAGED_CODE();

    status = WdfIoTargetCreate((WDFDEVICE)WdfObjectContextGetObject(pDeviceContext),
        WDF_NO_OBJECT_ATTRIBUTES,
        &hidTarget);

    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfIoTargetCreate failed 0x%x\n", status));
        return status;
    }

    //
    // Create a report to send to the device.
    //
    report = (PCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(TailLightReport), 'TLRp');

    if (report == NULL) {
        goto ExitAndFree;
    }

    TailLightReport* pTailLightReport = (TailLightReport*)report;
    pTailLightReport->Blue = 0;
    pTailLightReport->Green = 0;
    pTailLightReport->Red = 0;
  
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor,
        report,
        sizeof(TailLightReport));

    status = WdfIoTargetSendIoctlSynchronously(hidTarget,
        NULL,
        IOCTL_HID_SET_FEATURE,
        &inputDescriptor,
        NULL,
        NULL,
        NULL);

    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfIoTargetSendIoctlSynchronously failed 0x%x\n", status));
        goto ExitAndFree;
    }

ExitAndFree:
    if (report != NULL) {
        ExFreePool(report);
        report = NULL;
    }

    if (hidTarget != NULL) {
        WdfObjectDelete(hidTarget);
    }

    return status;
}

VOID EvtIoDeviceControlFilter(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
)
/*++
Routine Description:
    This event callback function is called when the driver receives an

    (KMDF) IOCTL_HID_Xxx code when handlng IRP_MJ_INTERNAL_DEVICE_CONTROL
    (UMDF) IOCTL_HID_Xxx, IOCTL_UMDF_HID_Xxx when handling IRP_MJ_DEVICE_CONTROL

Arguments:
    Queue - A handle to the queue object that is associated with the I/O request

    Request - A handle to a framework request object.

    OutputBufferLength - The length, in bytes, of the request's output buffer,
            if an output buffer is available.

    InputBufferLength - The length, in bytes, of the request's input buffer, if
            an input buffer is available.

    IoControlCode - The driver or system defined IOCTL associated with the request
--*/
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    //KdPrint(("TailLight: EvtIoDeviceControl (IoControlCode=0x%x, InputBufferLength=%Iu)\n", IoControlCode, InputBufferLength));

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);

    NTSTATUS status = STATUS_SUCCESS; //unhandled
    switch (IoControlCode) {
      case IOCTL_HID_SET_FEATURE: // 0xb0191
        status = SetFeatureFilter(device, Request, InputBufferLength);
        break;
    }
    // No NT_SUCCESS(status) check here since we don't want to fail blocked calls

    // Forward the request down the driver stack
    WDF_REQUEST_SEND_OPTIONS options = {};
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    BOOLEAN ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options);
    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        KdPrint(("TailLight: WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
}
