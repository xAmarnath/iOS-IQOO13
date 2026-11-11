/** @file
  Shared DMA mappings for protected Gunyah guests.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "VirtioMmioDevice.h"
#include "GunyahDmaPool.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

typedef struct {
  VOID                  *Original;
  VOID                  *Shared;
  UINTN                 Bytes;
  UINTN                 Pages;
  VIRTIO_MAP_OPERATION   Operation;
} GUNYAH_DMA_MAPPING;

EFI_STATUS
EFIAPI
VirtioMmioMapSharedBuffer (
  IN      VIRTIO_DEVICE_PROTOCOL  *This,
  IN      VIRTIO_MAP_OPERATION    Operation,
  IN      VOID                   *HostAddress,
  IN OUT  UINTN                  *NumberOfBytes,
  OUT     EFI_PHYSICAL_ADDRESS   *DeviceAddress,
  OUT     VOID                   **Mapping
  )
{
  EFI_STATUS          Status;
  GUNYAH_DMA_MAPPING  *Map;

  if ((HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL) ||
      (*NumberOfBytes == 0) ||
      (*NumberOfBytes > MAX_UINTN - (EFI_PAGE_SIZE - 1)) ||
      ((UINTN)HostAddress > MAX_UINTN - (*NumberOfBytes - 1)) ||
      ((Operation != VirtioOperationBusMasterRead) &&
       (Operation != VirtioOperationBusMasterWrite) &&
       (Operation != VirtioOperationBusMasterCommonBuffer)))
  {
    return EFI_INVALID_PARAMETER;
  }

  *Mapping = NULL;
  *DeviceAddress = 0;
  Status = GunyahDmaPoolInit ();
  if (Status == EFI_NOT_FOUND) {
    // An unprotected VM has no restricted pool.
    *DeviceAddress = (UINTN)HostAddress;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    // A present but unusable pool must never fall back to private RAM.
    return Status;
  }

  if (GunyahDmaPoolContains (HostAddress, *NumberOfBytes)) {
    MemoryFence ();
    *DeviceAddress = (UINTN)HostAddress;
    return EFI_SUCCESS;
  }
  if (Operation == VirtioOperationBusMasterCommonBuffer) {
    // Common buffers are simultaneously visible to CPU and device.
    // A bounce copy cannot provide those semantics.
    return EFI_UNSUPPORTED;
  }

  Map = AllocateZeroPool (sizeof (*Map));
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Map->Pages = EFI_SIZE_TO_PAGES (*NumberOfBytes);
  Status = GunyahDmaPoolAllocatePages (Map->Pages, &Map->Shared);
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return Status;
  }
  Map->Original = HostAddress;
  Map->Bytes = *NumberOfBytes;
  Map->Operation = Operation;
  ZeroMem (Map->Shared, EFI_PAGES_TO_SIZE (Map->Pages));
  if (Operation == VirtioOperationBusMasterRead) {
    CopyMem (Map->Shared, HostAddress, Map->Bytes);
  }
  MemoryFence ();
  *DeviceAddress = (UINTN)Map->Shared;
  *Mapping = Map;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
VirtioMmioUnmapSharedBuffer (
  IN VIRTIO_DEVICE_PROTOCOL  *This,
  IN VOID                   *Mapping
  )
{
  GUNYAH_DMA_MAPPING *Map;

  MemoryFence ();
  if (Mapping == NULL) {
    return EFI_SUCCESS;
  }
  Map = Mapping;
  if (Map->Operation == VirtioOperationBusMasterWrite) {
    CopyMem (Map->Original, Map->Shared, Map->Bytes);
  }
  ZeroMem (Map->Shared, EFI_PAGES_TO_SIZE (Map->Pages));
  GunyahDmaPoolFreePages (Map->Pages, Map->Shared);
  FreePool (Map);
  return EFI_SUCCESS;
}
