/*++

Copyright (c) Microsoft Corporation.  All rights reserved.

    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
    KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
    PURPOSE.

Module Name:

    driver.c

Abstract:

    Code for main entry point of KMDF driver

Author:


Environment:

    kernel mode only

Revision History:

--*/

#include <hidusbfx2.h>

#if defined(EVENT_TRACING)
//
// The trace message header (.tmh) file must be included in a source file
// before any WPP macro calls and after defining a WPP_CONTROL_GUIDS
// macro (defined in toaster.h). During the compilation, WPP scans the source
// files for DoTraceMessage() calls and builds a .tmh file which stores a unique
// data GUID for each message, the text resource string for each message,
// and the data types of the variables passed in for each message.  This file
// is automatically generated and used during post-processing.
//
#include "driver.tmh"
#else
ULONG DebugLevel = TRACE_LEVEL_INFORMATION;
ULONG DebugFlag = 0xff;
#endif

#ifdef ALLOC_PRAGMA
    #pragma alloc_text( INIT, DriverEntry )
    #pragma alloc_text( PAGE, HidFx2EvtDeviceAdd)
    #pragma alloc_text( PAGE, HidFx2EvtDriverContextCleanup)
#endif

//驱动名称：HIDUSBFX2 Driver Sample
NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:

    DriverObject - pointer to the driver object

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
{
    NTSTATUS               status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG      config;
    WDF_OBJECT_ATTRIBUTES  attributes;

    //
    // Initialize WPP Tracing
    //
    WPP_INIT_TRACING( DriverObject, RegistryPath );

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
        "HIDUSBFX2 Driver Sample\n");

    WDF_DRIVER_CONFIG_INIT(&config, HidFx2EvtDeviceAdd);

    //
    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = HidFx2EvtDriverContextCleanup;

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,      // Driver Attributes
                             &config,          // Driver Config Info
                             WDF_NO_HANDLE
                             );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
            "WdfDriverCreate failed with status 0x%x\n", status);
        
        WPP_CLEANUP(DriverObject);
    }

    return status;
}

//一个告知
//一个pnp回调设置
//一个hDevice
//两个queue
//一个timer，认hDevice为父亲
NTSTATUS
HidFx2EvtDeviceAdd(
    IN WDFDRIVER       Driver,
    IN PWDFDEVICE_INIT DeviceInit
    )
/*++
Routine Description:

    HidFx2EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a WDF device object to
    represent a new instance of toaster device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS                      status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG           queueConfig;
    WDF_OBJECT_ATTRIBUTES         attributes;
    WDFDEVICE                     hDevice;
    PDEVICE_EXTENSION             devContext = NULL;
    WDFQUEUE                      queue;
    WDF_PNPPOWER_EVENT_CALLBACKS  pnpPowerCallbacks;
    WDF_TIMER_CONFIG              timerConfig;
    WDFTIMER                      timerHandle;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
        "HidFx2EvtDeviceAdd called\n");

	//--------------------------------------------------------------------
	// 告诉这是一个filter driver
	//--------------------------------------------------------------------

    //
    // Tell framework this is a filter driver. Filter drivers by default are  
    // not power policy owners. This works well for this driver because
    // HIDclass driver is the power policy owner for HID minidrivers.
    //
    WdfFdoInitSetFilter(DeviceInit);

	//--------------------------------------------------------------------
	// 设置pnp回调
	//--------------------------------------------------------------------

    //
    // Initialize pnp-power callbacks, attributes and a context area for the device object.
    //
    //
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks); //本地变量

    //
    // For usb devices, PrepareHardware callback is the to place select the
    // interface and configure the device.
    //
    pnpPowerCallbacks.EvtDevicePrepareHardware = HidFx2EvtDevicePrepareHardware;//usb设备必须有，这里选择接口和配置设备

    //
    // These two callbacks start and stop the wdfusb pipe continuous reader
    // as we go in and out of the D0-working state.
    //
    pnpPowerCallbacks.EvtDeviceD0Entry = HidFx2EvtDeviceD0Entry;//进入D0，启动wdfusb pipe
    pnpPowerCallbacks.EvtDeviceD0Exit  = HidFx2EvtDeviceD0Exit; //退出D0，停止wdfusb pipe

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	//--------------------------------------------------------------------
	// 创建设备hDevice，并获得devContext
	//--------------------------------------------------------------------
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_EXTENSION);

    //
    // Create a framework device object.This call will in turn create
    // a WDM device object, attach to the lower stack, and set the
    // appropriate flags and attributes.
    //
    status = WdfDeviceCreate(&DeviceInit, &attributes, &hDevice/*输出，本地变量*/);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfDeviceCreate failed with status code 0x%x\n", status);
        return status;
    }
	//问题1：创建的hDevice保存在哪里了？
	//答案1：不用保存，贴在下一层fdo上面了
	//问题2：取出devContext有什么用？
	//答案2：下面保存人工queue对象和timer对象，这是我们自己搞出来的，特别是timer还是dDeivce的儿子

    devContext = GetDeviceContext(hDevice);//看出来hDevice有什么用，或者说WdfDeviceCreate有什么用？
    
	//--------------------------------------------------------------------
	// 创建第一个queue
	//--------------------------------------------------------------------
	// 问题:创建的queue放在哪儿了？
	// 答案:附加到hDevice了，也不用保存
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel/*枚举，并行好啊*/);
    queueConfig.EvtIoInternalDeviceControl = HidFx2EvtInternalDeviceControl;//没有这个恐怕真不行

    status = WdfIoQueueCreate(hDevice,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &queue
                              );
    if (!NT_SUCCESS (status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfIoQueueCreate failed 0x%x\n", status);
        return status;
    }

	//--------------------------------------------------------------------
	// 创建第二个queue，登记在devContext下面
	//--------------------------------------------------------------------

    //
    // Register a manual I/O queue for handling Interrupt Message Read Requests.
    // This queue will be used for storing Requests that need to wait for an
    // interrupt to occur before they can be completed.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    //
    // This queue is used for requests that dont directly access the device. The
    // requests in this queue are serviced only when the device is in a fully
    // powered state and sends an interrupt. So we can use a non-power managed
    // queue to park the requests since we dont care whether the device is idle
    // or fully powered up.
    //
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(hDevice,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &devContext->InterruptMsgQueue
                              );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfIoQueueCreate failed 0x%x\n", status);
        return status;
    }

	//--------------------------------------------------------------------
	// 创建timer，认hDevice为父亲，登记在devContext下面
	//--------------------------------------------------------------------

    //
    // Create a timer to handle debouncing of switchpack 
    //
    WDF_TIMER_CONFIG_INIT(
                          &timerConfig, //本地变量，第一次用，被初始化
                          HidFx2EvtTimerFunction //定时器到期就执行这个
                          );
    timerConfig.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); //重复使用
    attributes.ParentObject = hDevice; //timer好像必须都得有个父亲
    status = WdfTimerCreate(
                            &timerConfig,
                            &attributes,
                            &timerHandle //输出，将来保存到devContext中
                            );
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "WdfTimerCreate failed status:0x%x\n", status);
        return status;
    }

    devContext->DebounceTimer = timerHandle;
    return status;
}


VOID
HidFx2EvtDriverContextCleanup(
    IN WDFOBJECT Object
    )
/*++
Routine Description:

    Free resources allocated in DriverEntry that are not automatically
    cleaned up framework.

Arguments:

    Driver - handle to a WDF Driver object.

Return Value:

    VOID.

--*/
{
    PAGED_CODE ();
    UNREFERENCED_PARAMETER(Object);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Exit HidFx2EvtDriverContextCleanup\n");

    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER) Object));
}


#if !defined(EVENT_TRACING)

VOID
TraceEvents    (
    IN ULONG   TraceEventsLevel,
    IN ULONG   TraceEventsFlag,
    IN PCCHAR  DebugMessage,
    ...
    )

/*++

Routine Description:

    Debug print for the sample driver.

Arguments:

    TraceEventsLevel - print level between 0 and 3, with 3 the most verbose

Return Value:

    None.

 --*/
 {
#if DBG
#define     TEMP_BUFFER_SIZE        512
    va_list    list;
    CHAR       debugMessageBuffer[TEMP_BUFFER_SIZE];
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) {

        //
        // Using new safe string functions instead of _vsnprintf.
        // This function takes care of NULL terminating if the message
        // is longer than the buffer.
        //
        status = RtlStringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      DebugMessage,
                                      list );
        if(!NT_SUCCESS(status)) {

            DbgPrint (_DRIVER_NAME_": RtlStringCbVPrintfA failed 0x%x\n", status);
            return;
        }
        if (TraceEventsLevel <= TRACE_LEVEL_ERROR ||
            (TraceEventsLevel <= DebugLevel &&
             ((TraceEventsFlag & DebugFlag) == TraceEventsFlag))) {
            DbgPrint("%s%s", _DRIVER_NAME_, debugMessageBuffer);
        }
    }
    va_end(list);

    return;
#else
    UNREFERENCED_PARAMETER(TraceEventsLevel);
    UNREFERENCED_PARAMETER(TraceEventsFlag);
    UNREFERENCED_PARAMETER(DebugMessage);
#endif
}

#endif

