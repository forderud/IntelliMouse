#include "driver.h"
#include <Hidport.h>
#include "SetBlack.h"

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControlFilter;


NTSTATUS PnpNotifyDeviceInterfaceChange(
    _In_ PVOID pvNotificationStructure,
    _Inout_opt_ PVOID pvContext)
/*++
Routine Description:
    Filters out HID class device interface arrivals.

Arguments:
    PVOID - One of several possible notification structures. All we care about
            for this implementation is DEVICE_INTERFACE_CHANGE_NOTIFICATION.

    PVOID - The WDFDEVICE that we received from EvtDriverDeviceAdd.

--*/

{

    //KdPrint(("TailLight: PnpNotifyDeviceInterfaceChange enter\n"));
    NT_ASSERTMSG("WDFDEVICE not passed in!", pvContext);

    if (pvNotificationStructure == NULL) {
        return STATUS_SUCCESS;
    }

    PDEVICE_INTERFACE_CHANGE_NOTIFICATION pDevInterface =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)pvNotificationStructure;

    ASSERT(IsEqualGUID(pDevInterface->InterfaceClassGuid,
        GUID_DEVINTERFACE_HID));

    {     
        // Assumption: Device will arrive before removal.
        if (IsEqualGUID(pDevInterface->Event,
            GUID_DEVICE_INTERFACE_ARRIVAL)) {

            // Opening a device may trigger PnP operations. Ensure that either a
            // timer or a work item is used when opening up a device.
            // Refer to page 356 of "Programming The Microsoft Windows Driver
            // Model", 2nd edition by Walter Oney and IoGetDeviceObjectPointer.
            // 
            // Also note that eventually several system threads will be running
            // in parallel. As a PnP system thread must not block, serializing
            // would require a queue.
            //
            // NOTE: It is possible for us to get blocked waiting for a system
            // thread. One solution would be to use a timer that spawns a
            // system thread on timeout vs. a wait loop.
            //
            // What the operating system does with the return value is
            // undocumented. Since the docs say to return success or an error
            // value, follow the spec.
            return CreateWorkItemForIoTargetOpenDevice((WDFDEVICE)pvContext,
                *pDevInterface->SymbolicLinkName);
        }
    }

    return STATUS_SUCCESS;
}


NTSTATUS EvtSelfManagedIoInit(WDFDEVICE device) {

    WDFDRIVER driver = WdfDeviceGetDriver(device);
    DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

    NTSTATUS status = STATUS_SUCCESS;

    status = IoRegisterPlugPlayNotification(
        EventCategoryDeviceInterfaceChange,
        PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
        (PVOID)&GUID_DEVINTERFACE_HID,
        WdfDriverWdmGetDriverObject(driver),
        PnpNotifyDeviceInterfaceChange,
        (PVOID)device,
        &pDeviceContext->pnpDevInterfaceChangedHandle
    );

    return status;
}


UNICODE_STRING GetTargetPropertyString(WDFIOTARGET target, DEVICE_REGISTRY_PROPERTY DeviceProperty) {
    WDF_OBJECT_ATTRIBUTES attributes = {};
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = target; // auto-delete with I/O target

    WDFMEMORY memory = 0;
    NTSTATUS status = WdfIoTargetAllocAndQueryTargetProperty(target, DeviceProperty, NonPagedPoolNx, &attributes, &memory);
    if (!NT_SUCCESS(status)) {
        KdPrint(("TailLight: WdfIoTargetAllocAndQueryTargetProperty with property=0x%x failed 0x%x\n", DeviceProperty, status));
        return {};
    }

    // initialize string based on memory
    size_t bufferLength = 0;
    UNICODE_STRING result = {};
    result.Buffer = (WCHAR*)WdfMemoryGetBuffer(memory, &bufferLength);
    if (result.Buffer == NULL)
        return {};

    result.MaximumLength = (USHORT)bufferLength;
    result.Length = (USHORT)bufferLength - sizeof(UNICODE_NULL);
    return result;
}


VOID EvtCleanupCallback(
    _In_ WDFOBJECT object
)
/*++
Routine Description:
    Removes the plug and play notification callback if registered.
    Called near the end of processing an IRP_MN_REMOVE_DEVICE.
    This work could also be done at EvtDeviceSelfManagedIoCleanup, which
    comes before this callback. However using the lifetime of the device
    context means that the PnP notification handle will be around for all users
    of the device context, while the EvtDeviceSelfManagedIoCleanup would
    present a small time where the handle is "dangling." In addition, 
    IoRegisterPlugPlayNotification adds a reference to our device object. Thus,
    if the PnP notification handle is not removed, our driver will not unload
    and DriverUnload will not be called. Please see page 355 of "Programming
    The Microsoft Windows Driver Model", 2nd edition, by Walter Oney.

Arguments:
    WDFOBJECT - Handle to a framework device object from AddDevice
--*/
{
    DEVICE_CONTEXT* pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(object);

    if (pDeviceContext->pnpDevInterfaceChangedHandle) {
        NTSTATUS status = STATUS_SUCCESS;
        status = IoUnregisterPlugPlayNotificationEx(pDeviceContext->pnpDevInterfaceChangedHandle);
        if (!NT_SUCCESS(status)) {
            KdPrint(("IoUnregisterPlugPlayNotification failed with 0x%x\n", status));
        }
    }
    pDeviceContext->pnpDevInterfaceChangedHandle = NULL;
}


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

    {
        // register PnP callbacks (must be done before WdfDeviceCreate)
        WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
        PnpPowerCallbacks.EvtDeviceSelfManagedIoInit = EvtSelfManagedIoInit;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);
    }

    WDFDEVICE device = 0;
    {
        // create device
        WDF_OBJECT_ATTRIBUTES attributes = {};
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
        attributes.EvtCleanupCallback = EvtCleanupCallback;

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
        deviceContext->PdoName = GetTargetPropertyString(WdfDeviceGetIoTarget(device), DevicePropertyPhysicalDeviceObjectName);
        if (!deviceContext->PdoName.Buffer) {
            KdPrint(("TailLight: PdoName query failed\n"));
            return STATUS_UNSUCCESSFUL;
        }

        KdPrint(("TailLight: PdoName: %wZ\n", deviceContext->PdoName)); // outputs "\Device\00000083"
    }

    {
        // create queue for filtering
        WDF_IO_QUEUE_CONFIG queueConfig = {};
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel); // don't synchronize
        //queueConfig.EvtIoRead  // pass-through read requests 
        //queueConfig.EvtIoWrite // pass-through write requests
        queueConfig.EvtIoDeviceControl = EvtIoDeviceControlFilter; // filter IOCTL requests

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

    (KMDF) IOCTL_HID_Xxx code when handling IRP_MJ_INTERNAL_DEVICE_CONTROL
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
        KdPrint(("TailLight: WdfRequestSend failed with status: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
}