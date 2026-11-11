/** @file

  Gunyah restricted DMA pool allocator implementation.

  Discovers the "restricted-dma-pool" region from FDT and manages page-level
  allocations from it using a bitmap allocator.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/FdtClient.h>

#include "GunyahDmaPool.h"

//
// DMA pool state
//
STATIC EFI_STATUS            mPoolInitStatus = EFI_NOT_FOUND;
STATIC BOOLEAN               mPoolInitDone;
STATIC BOOLEAN               mPoolAvailable;
STATIC EFI_PHYSICAL_ADDRESS  mPoolBase;
STATIC UINTN                 mPoolTotalPages;

//
// Bitmap allocator: one bit per page. Bit set = page allocated.
// Max pool 256 MB → 65536 pages → 8192 bytes bitmap.  Heap-allocated.
//
STATIC UINT8   *mBitmap;
STATIC UINTN   mBitmapBytes;

STATIC
BOOLEAN
BitmapTestBit (
  IN UINTN  PageIndex
  )
{
  return (mBitmap[PageIndex / 8] & (1U << (PageIndex % 8))) != 0;
}

STATIC
VOID
BitmapSetBit (
  IN UINTN  PageIndex
  )
{
  mBitmap[PageIndex / 8] |= (UINT8)(1U << (PageIndex % 8));
}

STATIC
VOID
BitmapClearBit (
  IN UINTN  PageIndex
  )
{
  mBitmap[PageIndex / 8] &= (UINT8)~(1U << (PageIndex % 8));
}

/**
  Find a contiguous run of free pages in the bitmap.

  @param[in]  NumPages   Number of contiguous pages needed.
  @param[out] StartPage  Index of the first page in the run.

  @retval TRUE   Found.
  @retval FALSE  Not enough contiguous space.
**/
STATIC
BOOLEAN
BitmapFindFreeRun (
  IN  UINTN  NumPages,
  OUT UINTN  *StartPage
  )
{
  UINTN  RunStart;
  UINTN  RunLen;
  UINTN  i;

  RunStart = 0;
  RunLen   = 0;

  for (i = 0; i < mPoolTotalPages; i++) {
    if (BitmapTestBit (i)) {
      RunLen   = 0;
      RunStart = i + 1;
    } else {
      RunLen++;
      if (RunLen >= NumPages) {
        *StartPage = RunStart;
        return TRUE;
      }
    }
  }

  return FALSE;
}

EFI_STATUS
GunyahDmaPoolInit (
  VOID
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  CONST UINT64         *RegProp;
  UINT32               RegSize;
  UINT64               Base;
  UINT64               Size;

  if (mPoolInitDone) {
    return mPoolInitStatus;
  }

  mPoolInitDone  = TRUE;
  mPoolAvailable = FALSE;

  //
  // Locate FDT client protocol
  //
  Status = gBS->LocateProtocol (
                  &gFdtClientProtocolGuid,
                  NULL,
                  (VOID **)&FdtClient
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: FDT client not available\n", __func__));
    return EFI_NOT_FOUND;
  }

  //
  // Find the restricted-dma-pool node in FDT.
  // The FDT structure is:
  //   /reserved-memory {
  //     restricted_dma_reserved {
  //       compatible = "restricted-dma-pool";
  //       reg = <base_hi base_lo size_hi size_lo>;
  //     };
  //   };
  //
  Status = FdtClient->FindCompatibleNode (
                        FdtClient,
                        "restricted-dma-pool",
                        &Node
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "%a: restricted-dma-pool not found in FDT\n", __func__));
    return EFI_NOT_FOUND;
  }

  // Once the pool is found, malformed metadata must fail closed.
  mPoolInitStatus = EFI_DEVICE_ERROR;
  Status = FdtClient->GetNodeProperty (
                        FdtClient,
                        Node,
                        "reg",
                        (CONST VOID **)&RegProp,
                        &RegSize
                        );
  if (EFI_ERROR (Status) || (RegSize < sizeof (UINT64) * 2)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to read 'reg' from restricted-dma-pool (Status=%r, Size=%u)\n",
      __func__,
      Status,
      RegSize
      ));
    return mPoolInitStatus;
  }

  //
  // Parse reg property.  Gunyah uses #address-cells=2, #size-cells=2
  // so each entry is two big-endian UINT64 values.
  //
  Base = SwapBytes64 (RegProp[0]);
  Size = SwapBytes64 (RegProp[1]);

  if ((Size == 0) || (Base == 0) ||
      ((Base | Size) & (EFI_PAGE_SIZE - 1)) ||
      (Base > MAX_UINTN) || (Size > MAX_UINTN - Base)) {
    DEBUG ((DEBUG_ERROR, "%a: Invalid DMA pool region (Base=0x%lx Size=0x%lx)\n",
            __func__, Base, Size));
    return mPoolInitStatus;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: restricted-dma-pool @ 0x%lx size 0x%lx (%lu MB)\n",
    __func__,
    Base,
    Size,
    Size / (1024 * 1024)
    ));

  mPoolBase       = Base;
  mPoolTotalPages = EFI_SIZE_TO_PAGES (Size);

  //
  // Allocate bitmap (1 bit per page)
  //
  mBitmapBytes = (mPoolTotalPages + 7) / 8;
  mBitmap      = AllocateZeroPool (mBitmapBytes);
  if (mBitmap == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to allocate bitmap (%u bytes)\n",
            __func__, mBitmapBytes));
    mPoolInitStatus = EFI_OUT_OF_RESOURCES;
    return mPoolInitStatus;
  }

  mPoolInitStatus = EFI_SUCCESS;
  mPoolAvailable = TRUE;
  return EFI_SUCCESS;
}

BOOLEAN
GunyahDmaPoolIsAvailable (
  VOID
  )
{
  if (!mPoolInitDone) {
    GunyahDmaPoolInit ();
  }

  return mPoolAvailable;
}

EFI_STATUS
GunyahDmaPoolAllocatePages (
  IN  UINTN  NumPages,
  OUT VOID   **HostAddress
  )
{
  UINTN  StartPage;
  UINTN  i;

  if (!mPoolAvailable) {
    return EFI_NOT_FOUND;
  }

  if ((HostAddress == NULL) || (NumPages == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (!BitmapFindFreeRun (NumPages, &StartPage)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: DMA pool exhausted (requested %u pages)\n",
      __func__,
      NumPages
      ));
    return EFI_OUT_OF_RESOURCES;
  }

  for (i = StartPage; i < StartPage + NumPages; i++) {
    BitmapSetBit (i);
  }

  *HostAddress = (VOID *)(UINTN)(mPoolBase + EFI_PAGES_TO_SIZE (StartPage));

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: Allocated %u pages @ 0x%p from DMA pool\n",
    __func__,
    NumPages,
    *HostAddress
    ));

  return EFI_SUCCESS;
}

VOID
GunyahDmaPoolFreePages (
  IN UINTN  NumPages,
  IN VOID   *HostAddress
  )
{
  UINTN  PageOffset;
  UINTN  StartPage;
  UINTN  i;

  if (!mPoolAvailable || (HostAddress == NULL)) {
    return;
  }

  if (((UINTN)HostAddress < mPoolBase) ||
      (((UINTN)HostAddress - mPoolBase) & (EFI_PAGE_SIZE - 1))) {
    return;
  }
  PageOffset = (UINTN)HostAddress - (UINTN)mPoolBase;
  StartPage  = PageOffset / EFI_PAGE_SIZE;

  if ((StartPage > mPoolTotalPages) || (NumPages > mPoolTotalPages - StartPage)) {
    DEBUG ((DEBUG_ERROR, "%a: Free out of range @ %p\n", __func__, HostAddress));
    return;
  }

  for (i = StartPage; i < StartPage + NumPages; i++) {
    BitmapClearBit (i);
  }

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: Freed %u pages @ 0x%p back to DMA pool\n",
    __func__,
    NumPages,
    HostAddress
    ));
}

BOOLEAN
GunyahDmaPoolContains (
  IN VOID   *Address,
  IN UINTN  Bytes
  )
{
  UINTN Offset;
  UINTN PoolSize;

  if (!mPoolAvailable || ((UINTN)Address < mPoolBase) || (Bytes == 0)) {
    return FALSE;
  }
  Offset = (UINTN)Address - (UINTN)mPoolBase;
  PoolSize = EFI_PAGES_TO_SIZE (mPoolTotalPages);
  return (Offset < PoolSize) && (Bytes <= PoolSize - Offset);
}
