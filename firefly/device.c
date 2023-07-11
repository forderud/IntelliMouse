#include "FireFly.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FireFlyEvtDeviceAdd)
#endif

NTSTATUS
FireFlyEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
/*++
Routine Description:
    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a device object to
    represent to be part of the device stack as a filter.

Arguments:
    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:
    NTSTATUS

--*/    
{
    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    // Configure the device as a filter driver
    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES attributes = {0};
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    WDFDEVICE device = 0;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfDeviceCreate, Error %x\n", status));
        return status;
    }

    // Driver Framework always zero initializes an objects context memory
    PDEVICE_CONTEXT pDeviceContext = WdfObjectGet_DEVICE_CONTEXT(device);

    // Initialize our WMI support
    status = WmiInitialize(device, pDeviceContext);
    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: Error initializing WMI 0x%x\n", status));
        return status;
    }

    // In order to send ioctls to our PDO, we have open to open it
    // by name so that we have a valid filehandle (fileobject).
    // When we send ioctls using the IoTarget, framework automatically 
    // sets the filobject in the stack location.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    // By parenting it to device, we don't have to worry about
    // deleting explicitly. It will be deleted along witht the device.
    attributes.ParentObject = device;

    WDFMEMORY memory = 0;
    status = WdfDeviceAllocAndQueryProperty(device,
                                    DevicePropertyPhysicalDeviceObjectName,
                                    NonPagedPoolNx,
                                    &attributes,
                                    &memory);

    if (!NT_SUCCESS(status)) {
        KdPrint(("FireFly: WdfDeviceAllocAndQueryProperty failed 0x%x\n", status));        
        return STATUS_UNSUCCESSFUL;
    }

    {
        // initialize pDeviceContext->PdoName based on memory
        size_t bufferLength = 0;
        pDeviceContext->PdoName.Buffer = WdfMemoryGetBuffer(memory, &bufferLength);
        if (pDeviceContext->PdoName.Buffer == NULL)
            return STATUS_UNSUCCESSFUL;

        pDeviceContext->PdoName.MaximumLength = (USHORT)bufferLength;
        pDeviceContext->PdoName.Length = (USHORT)bufferLength - sizeof(UNICODE_NULL);
    }

    return status;
}
