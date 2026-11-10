 #include <ntddk.h>
#define DRIVER_TAG '0baL'

typedef struct ListTaha {
	ULONG Size;
	ULONG Pos;
	LIST_ENTRY Link;
} ListTaha, *PLIST_TAHA;

ListTaha* GetItem(PLIST_ENTRY pEntry) {
	return CONTAINING_RECORD(pEntry, ListTaha, Link);
}

static LIST_ENTRY g_Head;
static FAST_MUTEX g_Lock;
static ULONG      g_Count = 0;

static UNICODE_STRING g_DevName = RTL_CONSTANT_STRING(L"\\Device\\TahaList");
static UNICODE_STRING g_SymName = RTL_CONSTANT_STRING(L"\\??\\TahaList");

#define IOCTL_TAHA_GET_COUNT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TAHA_ADD       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

static void DrainList() {
	ExAcquireFastMutex(&g_Lock);
	while (!IsListEmpty(&g_Head)) {
		PLIST_ENTRY pLink = RemoveHeadList(&g_Head);
		PLIST_TAHA  pItem = GetItem(pLink);
		KdPrint(("Removing item, Pos=%u\n", pItem->Pos));
		ExFreePoolWithTag(pItem, DRIVER_TAG);
		g_Count--;
	}
	ExReleaseFastMutex(&g_Lock);
}
static NTSTATUS AddItem(ULONG value) {
	PLIST_TAHA pEntry = (PLIST_TAHA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ListTaha), DRIVER_TAG);
	if (pEntry == nullptr)
		return STATUS_INSUFFICIENT_RESOURCES;

	ExAcquireFastMutex(&g_Lock);
	pEntry->Size = value;
	pEntry->Pos = g_Count;          // position = its index at insert time
	InsertTailList(&g_Head, &pEntry->Link);
	g_Count++;
	ExReleaseFastMutex(&g_Lock);
	return STATUS_SUCCESS;
}

static NTSTATUS OnCreateClose(_In_ PDEVICE_OBJECT dev, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(dev);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

static NTSTATUS OnDeviceControl(_In_ PDEVICE_OBJECT dev, _In_ PIRP Irp) {
	UNREFERENCED_PARAMETER(dev);
	auto sl = IoGetCurrentIrpStackLocation(Irp);
	auto& dic = sl->Parameters.DeviceIoControl;
	NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;
	ULONG_PTR info = 0;

	switch (dic.IoControlCode) {
	case IOCTL_TAHA_GET_COUNT:
		if (dic.OutputBufferLength < sizeof(ULONG)) { st = STATUS_BUFFER_TOO_SMALL; break; }
		ExAcquireFastMutex(&g_Lock);
		*(ULONG*)Irp->AssociatedIrp.SystemBuffer = g_Count;
		ExReleaseFastMutex(&g_Lock);
		info = sizeof(ULONG);
		st = STATUS_SUCCESS;
		break;

	case IOCTL_TAHA_ADD:
		if (dic.InputBufferLength < sizeof(ULONG)) { st = STATUS_BUFFER_TOO_SMALL; break; }
		st = AddItem(*(ULONG*)Irp->AssociatedIrp.SystemBuffer);
		break;
	}

	Irp->IoStatus.Status = st;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return st;
}

static void DriverUnload(_In_ PDRIVER_OBJECT DriverObject) {
	IoDeleteSymbolicLink(&g_SymName);
	if (DriverObject->DeviceObject)
		IoDeleteDevice(DriverObject->DeviceObject);
	DrainList();                       // <-- the SAME global list, not a local
	KdPrint(("Driver unloaded!\n"));
}

extern "C"
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);

	// (1) initialize the GLOBAL list + lock before anything can use them
	InitializeListHead(&g_Head);
	ExInitializeFastMutex(&g_Lock);
	g_Count = 0;

	// seed 3 nodes - note: NO while-loop teardown here anymore
	for (ULONG i = 0; i < 3; i++) {
		NTSTATUS s = AddItem(i);
		if (!NT_SUCCESS(s)) { DrainList(); return s; }  // unwind on failure
		KdPrint(("Seeded item %u\n", i));
	}

	// (3) create the device object + symbolic link = the doorway
	PDEVICE_OBJECT dev;
	NTSTATUS st = IoCreateDevice(DriverObject, 0, &g_DevName,
		FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
	if (!NT_SUCCESS(st)) { DrainList(); return st; }

	st = IoCreateSymbolicLink(&g_SymName, &g_DevName);
	if (!NT_SUCCESS(st)) { IoDeleteDevice(dev); DrainList(); return st; }

	// wire the dispatch table so IRPs reach your routines
	DriverObject->DriverUnload = DriverUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = OnCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = OnCreateClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = OnDeviceControl;

	KdPrint(("Driver loaded! list has %u items\n", g_Count));
	return STATUS_SUCCESS;             // <-- the list STAYS ALIVE now
}
