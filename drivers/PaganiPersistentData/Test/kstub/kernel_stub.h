// SPDX-License-Identifier: BSD-2-Clause-Patent
// Declarations only: enough of IOKit/libkern/pexpert for a host -fsyntax-only check of NativeRamBlock.cpp before and after
// integration/NativeRamBlock.patch (Test/run.sh). Nothing here is linked or run.
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
typedef uint64_t UInt64;
typedef uint32_t UInt32;
typedef int IOReturn;
typedef uint32_t IOOptionBits;
enum : IOReturn { kIOReturnSuccess = 0, kIOReturnBadArgument, kIOReturnIOError, kIOReturnNoMemory, kIOReturnNotReady, kIOReturnUnsupported };
enum IODirection { kIODirectionNone = 0, kIODirectionIn = 1, kIODirectionOut = 2 };
struct IOLock;
IOLock *IOLockAlloc ();
void IOLockFree (IOLock *);
void IOLockLock (IOLock *);
void IOLockUnlock (IOLock *);
void *IOMallocZero (size_t);
void IOFree (void *, size_t);
void IOLog (const char *, ...) __attribute__ ((format (printf, 1, 2)));
extern "C" int PE_parse_boot_argn (const char *, void *, int);
uintptr_t ml_static_ptovirt (uint64_t);
struct SHA256_CTX { uint64_t opaque[16]; };
void SHA256_Init (SHA256_CTX *);
void SHA256_Update (SHA256_CTX *, const void *, size_t);
void SHA256_Final (void *, SHA256_CTX *);
class OSObject { public: virtual ~OSObject () {} virtual void release () const {} };
class OSString : public OSObject { public: static OSString *withCString (const char *); const char *getCStringNoCopy () const; unsigned getLength () const; };
class OSData : public OSObject { public: unsigned getLength () const; const void *getBytesNoCopy () const; };
class OSNumber : public OSObject { public: uint64_t unsigned64BitValue () const; uint32_t unsigned32BitValue () const; };
class OSDictionary : public OSObject { public: static OSDictionary *withCapacity (unsigned); bool setObject (const char *, const OSObject *); };
class OSIterator : public OSObject { public: OSObject *getNextObject (); };
#define OSDynamicCast(type, inst) (static_cast<type *> (const_cast<OSObject *> (static_cast<const OSObject *> (inst))))
#define OSSafeReleaseNULL(inst) do { if (inst) (inst)->release (); (inst) = nullptr; } while (0)
struct IORegistryPlane;
extern const IORegistryPlane *gIODTPlane;
class IORegistryEntry : public OSObject {
public:
  static IORegistryEntry *fromPath (const char *, const IORegistryPlane *);
  OSObject *getProperty (const char *) const;
};
class IOService : public IORegistryEntry {
public:
  virtual bool start (IOService *);
  virtual void stop (IOService *);
  virtual void free ();
  void registerService ();
  IOService *getProvider () const;
  static OSDictionary *serviceMatching (const char *);
  static OSIterator *getMatchingServices (OSDictionary *);
};
class IOMemoryDescriptor : public OSObject {
public:
  uint64_t readBytes (uint64_t, void *, uint64_t);
  uint64_t writeBytes (uint64_t, const void *, uint64_t);
  uint64_t getLength () const;
  IODirection getDirection () const;
};
struct IOStorageAttributes { uint32_t options; };
typedef void (*IOStorageCompletionAction) (void *target, void *parameter, IOReturn status, UInt64 actualByteCount);
struct IOStorageCompletion { void *target; IOStorageCompletionAction action; void *parameter; };
class IOBlockStorageDevice : public IOService {
public:
  virtual IOReturn doEjectMedia () = 0;
  virtual IOReturn doFormatMedia (UInt64) = 0;
  virtual UInt32 doGetFormatCapacities (UInt64 *, UInt32) const = 0;
  virtual char *getVendorString () = 0;
  virtual char *getProductString () = 0;
  virtual char *getRevisionString () = 0;
  virtual char *getAdditionalDeviceInfoString () = 0;
  virtual IOReturn reportBlockSize (UInt64 *) = 0;
  virtual IOReturn reportEjectability (bool *) = 0;
  virtual IOReturn reportMaxValidBlock (UInt64 *) = 0;
  virtual IOReturn reportMediaState (bool *, bool *) = 0;
  virtual IOReturn reportRemovability (bool *) = 0;
  virtual IOReturn reportWriteProtection (bool *) = 0;
  virtual IOReturn doSynchronizeCache () = 0;
  virtual IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) = 0;
  virtual IOReturn getWriteCacheState (bool *) = 0;
  virtual IOReturn setWriteCacheState (bool) = 0;
  virtual IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) = 0;
};
#define OSDeclareDefaultStructors(className) public: className (); private:
#define OSDefineMetaClassAndStructors(className, superclass) className::className () {}
#define kIOBSDNameKey "BSD Name"
#define kIOBSDMajorKey "BSD Major"
#define kIOBSDMinorKey "BSD Minor"
#ifndef NODEV
#define NODEV ((dev_t) -1)
#endif
