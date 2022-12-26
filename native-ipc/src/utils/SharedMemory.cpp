#include "common/EndTime.h"
#include "common/kog_exports.h"

#include "utils/log.h"
#include "utils/errHandling.h"

#include <thread>

#include <fcntl.h>

#ifdef LOCKING
#include <semaphore.h>
#endif
#ifndef _MSC_VER
#include <sys/mman.h>
#include <unistd.h>
#else
#include <windows.h>
// The stupid ass windows #defined ERROR
#undef ERROR
#endif

#include "utils/SharedMemory.h"

#include "utils/cvtypes.h"

#define COMPONENT "SHMQ"
#define PCV4K_IPC_TRACE RAW_PCV4J_IPC_TRACE(COMPONENT)

// =========================================================
// shorthand for some ubiquitous checks
#ifdef NO_CHECKS
#define MAILBOX_CHECK(h, x)
#define OPEN_CHECK(x)
#define NULL_CHECK(nr)
#else
#define MAILBOX_CHECK(h, x) if (x >= h->numMailboxes) { \
  log(ERROR, COMPONENT, "There are only %d mailboxes. You referenced mailbox %d", (int)header->numMailboxes, x); \
  return fromErrno(EINVAL); \
}

#define OPEN_CHECK(x)   if (! isOpen) { \
    log(ERROR, COMPONENT, "Cannot " x " until the shm segment is open"); \
    return fromErrorCode(NOT_OPEN); \
  }

#define NULL_CHECK(nr) if (nr == 0) { \
  log(ERROR, COMPONENT, "Null ShmQueue native reference passed"); \
  return fromErrorCode(NULL_REF); \
}
#endif
// =========================================================

using namespace std::chrono_literals;
using namespace ai::kognition::pilecv4j;

namespace pilecv4j {
namespace ipc {

static const uint64_t OK_RET = fromErrorCode(OK);

#define std_atomic_fence(x) std::atomic_thread_fence(x)

#define SHM_HEADER_MAGIC 0xBADFADE0CAFEF00D

#ifdef _MSC_VER
// Yes. I KNOW the zero length array wont participate in a copy or initialized by
// a default constructor. Thanks Bill.
#pragma warning( push )
#pragma warning( disable : 4200 )
#endif
struct Header {
  uint64_t magic = SHM_HEADER_MAGIC;
#ifdef LOCKING
  sem_t sem;
#endif
  std::size_t totalSize = 0;
  std::size_t numBytes = 0;
  std::size_t offset = 0;
  std::size_t numMailboxes = 0;
  std::size_t messageAvailable[0];
};
#ifdef _MSC_VER
#pragma warning( pop )
#endif

#ifdef LOCKING
static uint64_t lockMe(sem_t* sem, int64_t millis, bool aggressive) {
  PCV4K_IPC_TRACE;
  // ===========================================
  // try lockWrite
  if (millis == 0) {
    // try once performance boost
    if (sem_trywait(sem) == -1) {
      // this is okay if errno is EAGAIN
      int err = errno;
      if (err != EAGAIN) {
        char erroStr[256];
        const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
        erroStr[sizeof(erroStr) - 1] = (char)0;
        log(ERROR, COMPONENT, "Failed to open shared memory segment. Error %d: %s", err, msg);
      }
      return fromErrno(err);
    } else
      return OK_RET;
  }
  // ===========================================

  // if it's not just a poll, set up a timeout (which may be infinite).
  EndTime<std::chrono::milliseconds> endTime;
  if (millis > 0)
    endTime.set(std::chrono::milliseconds(millis));
  else
    endTime.setInfinite();

  while(!endTime.isTimePast()) {
    if (sem_trywait(sem) == -1) {
      // this is okay if errno is EAGAIN
      int err = errno;
      if (err != EAGAIN) {
        char erroStr[256];
        const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
        erroStr[sizeof(erroStr) - 1] = (char)0;
        log(ERROR, COMPONENT, "Failed to open shared memory segment. Error %d: %s", err, msg);
        return fromErrno(err);
      } else { // EAGAIN
        if (aggressive)
          std::this_thread::yield();
        else
          std::this_thread::sleep_for(1ms);
      }
    } else
      return OK_RET;
  }
  // if we got here, we timed out.
  return fromErrno(EAGAIN);
}

static uint64_t unlockMe(sem_t* sem) {
  PCV4K_IPC_TRACE;
  if (sem_post(sem) == -1) {
    int err = errno;
    char erroStr[256];
    const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
    erroStr[sizeof(erroStr) - 1] = (char)0;
    log(ERROR, COMPONENT, "Failed to open shared memory segment. Error %d: %s", err, msg);
    return fromErrno(err);
  }

  return OK_RET;
}
#else
#define unlockMe(x) OK_RET
#define lockMe(x,y,z) OK_RET
#endif

void SharedMemory::cleanup() {
  if (addr && totalSize >= 0) {
    if (!unmmapSharedMemorySegment(addr, totalSize)) {
      ErrnoType err = getLastError();
      std::string errMsgStr = getErrorMessage(err);
      log(ERROR, COMPONENT, "Failed to un-mmap the shared memory segment. Error %d: %s", (int)err, errMsgStr.c_str());
    }
    addr = nullptr;
    totalSize = -1;
#ifdef _MSC_VER
#else
    if (munmap(addr, totalSize) == -1) {
      int err = errno;
      char erroStr[256];
      const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
      erroStr[sizeof(erroStr) - 1] = (char)0;
      log(ERROR, COMPONENT, "Failed to un-mmap the shared memory segment. Error %d: %s", (int)err, msg);
    }
#endif
  }
  if (owner && fd >= 0) {
    unlink();
  }
}

uint64_t SharedMemory::unlink() {
  if (isEnabled(TRACE))
    log(TRACE,COMPONENT,"unlinking the shared memory segment %s.", name.c_str());

  if (fd < 0 || !isOpen) {
    log(ERROR, COMPONENT, "Attempt to unlink the shared memory segment \"%s\" but it's not currently open", name.c_str());
    return fromErrorCode(NOT_OPEN);
  }

  if (!owner)
    log(WARN, COMPONENT, "unlinking the shared memory segment \"%s\" though I'm not the owner.", name.c_str());

  if (!closeSharedMemorySegment(fd, name.c_str())) {
    ErrnoType err = getLastError();
    std::string errMsgStr = getErrorMessage(err);
    log(ERROR, COMPONENT, "Failed to close the shared memory segment. Error %d: %s", err, errMsgStr.c_str());
    return fromErrno(err);
  }
#ifdef _MSC_VER
#else
  if (shm_unlink(name.c_str())) {
    int err = errno;
    char erroStr[256];
    const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
    erroStr[sizeof(erroStr) - 1] = (char)0;
    log(ERROR, COMPONENT, "Failed to unlink the shared memory segment. Error %d: %s", err, msg);
    return fromErrno(err);
  }
#endif

  fd = PCV4J_IPC_DEFAULT_DESCRIPTOR;
  isOpen = false;

  return OK_RET;
}

uint64_t SharedMemory::create(std::size_t numBytes, bool powner, std::size_t numMailboxes) {
  if (isEnabled(DEBUG))
    log(DEBUG, COMPONENT, "Creating shared mem queue for %ld bytes. Owner: %s", (long)numBytes, powner ? "true": "false" );

  Header* hptr;
  std::string errMsgPrefix;
  std::size_t offsetToBuffer;

  // we need to round UP to the nearest 64 byte boundary
  offsetToBuffer = align64(sizeof(Header) + (numMailboxes * sizeof(std::size_t)));

  totalSize = align64(numBytes + offsetToBuffer);
  if (isEnabled(DEBUG))
    log(DEBUG, COMPONENT, "  the total size including the header is %ld bytes with an offset of %d", (long)totalSize, (int)offsetToBuffer);

  if (!createSharedMemorySegment(&fd, name.c_str(), totalSize)) {
      goto error;
  }

#ifndef _MSC_VER
  fd = shm_open(name.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);// <---+
  if (fd == -1){                                       //                 |
    errMsg = "Failed to open shared memory segment";   //                 |
    goto error;                                        //                 |
  }                                                    //                 |
  //       There is a race condition here which is "fixed" with a STUPID hack.
  //       The other process can open the shm segment now and mmap it before
  //       this process ftruncates it in which case access to the mapped memory
  //       will cause a seg fault. Therefore there's a stupid SLEEP in there
  //  +--- to minimize this gap.
  //  |
  //  v
  if (ftruncate(fd, totalSize) == -1)
    goto error;
#else
#endif

  this->owner = powner;

  if (!mmapSharedMemorySegment(&addr, fd, totalSize)) {
      errMsgPrefix = "Failed to map memory segment";
      goto error;
  }
#ifndef _MSC_VER
  // map shared memory to process address space
  addr = mmap(NULL, totalSize, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    errMsgPrefix = "Failed to map memory segment";
    goto error;
  }
#else
#endif

  hptr = (Header*)addr;
#ifdef LOCKING
  if (sem_init(&(hptr->sem), 1, 1) == -1) {
    errMsg = "Failed to init semaphore";
    goto error;
  }
#endif

  // set the sizes
  hptr->totalSize = totalSize;
  hptr->numBytes = numBytes;
  hptr->offset = offsetToBuffer;
  hptr->numMailboxes = numMailboxes;
  for (std::size_t i = 0; i < numMailboxes; i++)
    hptr->messageAvailable[i] = 0;

  data = ((uint8_t*)addr) + offsetToBuffer;
  if (isEnabled(DEBUG))
    log(DEBUG, COMPONENT, "Allocated shared mem at 0x%P with offset to data of %d bytes putting the data at 0x%P", addr, (int)offsetToBuffer, data);

  // don't reorder any write operation below this with any read/write operations above this line
  std::atomic_thread_fence(std::memory_order_release);
  // set the magic number
  hptr->magic = SHM_HEADER_MAGIC;
  this->isOpen = true;
  return OK_RET;

  error:
  {
      ErrnoType err = getLastError();
      std::string errMsgStr = getErrorMessage(err);
      log(ERROR, COMPONENT, "%s Error %d: %s", errMsgPrefix.c_str(), err, errMsgStr.c_str());
      return fromErrno(err);
#ifdef _MSC_VER
#else
    int err = errno;
    char erroStr[256];
    const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
    erroStr[sizeof(erroStr) - 1] = (char)0;
#endif
  }
  return false;
}

uint64_t SharedMemory::open(bool powner) {
  Header lheader;
  Header* header;
  EndTime<> endTime;
  ErrnoType err;

  if (!openSharedMemorySegment(&fd, name.c_str())) {
      err = getLastError();
      if (err == ENOENT) // no such file is a normal condition. Seems to be the same on windows and real OSes
          return fromErrno(EAGAIN);
      goto error;
  }
#ifdef _MSC_VER
#else
  fd = shm_open(name.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1) {
      err = errno;
      if (err == ENOENT) // no such file is a normal condition
          return fromErrno(EAGAIN);
      goto error;
  }
#endif

  // see the above described race condition. We need to allow the other process enough
  // time to get from shm_open to ftruncate. Other than tests, open is called infrequently
  // so this shouldn't be an overall performance hit.
  std::this_thread::sleep_for(200ms);

  void* tmpptr;
  if (!mmapSharedMemorySegment( &tmpptr, fd, sizeof(Header))) {
    err = getLastError();
    log(INFO, COMPONENT, "MapViewOfFile returned %d", err);
    goto error;
  }
  header = (Header*)tmpptr;
#ifdef _MSC_VER
#else
  // map just the header so we can see what the total size is.
  header = (Header*)mmap(nullptr, sizeof(Header), PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
  if (header == MAP_FAILED) {
    err = errno;
    goto error;
  }
#endif

  this->owner = powner;

  // poll for the magic number to be set.
  endTime.set(50ms);
  while(header->magic != SHM_HEADER_MAGIC && !endTime.isTimePast())
    std::this_thread::yield();

  if (header->magic != SHM_HEADER_MAGIC) {
    if (isEnabled(DEBUG))
      log(DEBUG, COMPONENT, "Timed out waiting for the serving size to setup the semaphore");
    return fromErrno(EAGAIN);
  }

  // don't reorder the read above this with any read/write below this
  std::atomic_thread_fence(std::memory_order_release);

  // okay, it's set up. re-map it to the correct size.
  lheader = *header; // copy the header

  if (!unmmapSharedMemorySegment(header, sizeof(Header))) {
    err = getLastError();
    goto error;
  }
#ifdef _MSC_VER
#else
  if (munmap(header, sizeof(Header)) == -1) {
    err = errno;
    goto error;
  }
#endif

  // now remap.
#ifdef _MSC_VER
  this->addr = MapViewOfFile(fd, // handle to map object
      FILE_MAP_ALL_ACCESS,       // read/write permission
      0,
      0,
      lheader.totalSize);
  if (!addr) {
    err = GetLastError();
#else
  this->addr = mmap(NULL, lheader.totalSize, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
  if (this->addr == MAP_FAILED) {
    err = errno;
#endif
    goto error;
  }
  this->data = ((uint8_t*)this->addr) + lheader.offset;
  this->totalSize = lheader.totalSize;
  this->isOpen = true;

  return OK_RET;
  error:
  {
    ErrnoType err = getLastError();
    std::string errMsgStr = getErrorMessage(err);
    log(WARN, COMPONENT, "Failed to open shared memory segment. Error %d: %s", err, errMsgStr.c_str());
#ifdef _MSC_VER
#else
    char erroStr[256];
    const char* msg = strerror_r(err, erroStr, sizeof(erroStr));
    erroStr[sizeof(erroStr) - 1] = (char)0;
    log(WARN, COMPONENT, "Failed to open shared memory segment. Error %d: %s", err, msg);
#endif
    return fromErrno(err);
  }
}

uint64_t SharedMemory::getBufferSize(std::size_t& out) {
  PCV4K_IPC_TRACE;
  OPEN_CHECK("get buffer size");

  Header* header = (Header*)addr;
  out = (std::size_t)header->numBytes;
  return OK_RET;
}

uint64_t SharedMemory::getBuffer(std::size_t offset, void*& out) {
  PCV4K_IPC_TRACE;
  OPEN_CHECK("get buffer");

  out = ((uint8_t*)this->data + offset);
  if (isEnabled(TRACE))
    log(TRACE, COMPONENT, "getBuffer is returning buffer at 0x%P", out);
  return OK_RET;
}

uint64_t SharedMemory::postMessage(std::size_t mailbox) {
  PCV4K_IPC_TRACE;
  OPEN_CHECK("post a message");

  Header* header = (Header*)addr;
  MAILBOX_CHECK(header, mailbox);
  // don't reorder any write operation below this with any read/write operations above this line
  std_atomic_fence(std::memory_order_release);
  header->messageAvailable[mailbox] = 1;
  return OK_RET;
}

uint64_t SharedMemory::unpostMessage(std::size_t mailbox) {
  PCV4K_IPC_TRACE;
  OPEN_CHECK("unpost a message");

  Header* header = (Header*)addr;
  MAILBOX_CHECK(header, mailbox);
  // don't reorder any write operation below this with any read/write operations above this line
  std_atomic_fence(std::memory_order_release);
  header->messageAvailable[mailbox] = 0;
  return OK_RET;
}

uint64_t SharedMemory::isMessageAvailable(bool& out, std::size_t mailbox) {
  PCV4K_IPC_TRACE;
  OPEN_CHECK("check if a message is available");

  Header* header = (Header*)addr;
  MAILBOX_CHECK(header, mailbox);

  //log(INFO, COMPONENT, "Header mailbox addr: 0x%llx : %llu", (void*)(&(header->messageAvailable[mailbox])), header->messageAvailable[mailbox]);
  out = header->messageAvailable[mailbox] ? true : false;
  // don't reorder the read above this with any read/write below this
  std_atomic_fence(std::memory_order_acquire);
  return OK_RET;
}

uint64_t SharedMemory::lock(int64_t millis, bool aggressive) {
  OPEN_CHECK("lock the shared memory segment");

  Header* header = (Header*)addr;

  return lockMe(&(header->sem), millis, aggressive);
}

uint64_t SharedMemory::unlock() {
  OPEN_CHECK("unlock the shared memory segment");

  Header* header = (Header*)addr;

  return unlockMe(&(header->sem));
}

extern "C" {

KAI_EXPORT uint64_t pilecv4j_ipc_create_shmQueue(const char* name) {
  PCV4K_IPC_TRACE;
  return (uint64_t)SharedMemory::instantiate(name);
}

KAI_EXPORT void pilecv4j_ipc_destroy_shmQueue(uint64_t nativeRef) {
  PCV4K_IPC_TRACE;
  if (nativeRef)
    delete (SharedMemory*)nativeRef;
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_create(uint64_t nativeRef, uint64_t size, int32_t owner, int32_t numMailboxes) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->create(size, owner == 0 ? false : true, (std::size_t)numMailboxes);
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_open(uint64_t nativeRef, int32_t owner) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->open(owner == 0 ? false : true);
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_unlink(uint64_t nativeRef) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->unlink();
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_bufferSize(uint64_t nativeRef, uint64_t* ret) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  if (!ret)
    return fromErrno(EINVAL);
  return (uint64_t)((SharedMemory*)nativeRef)->getBufferSize(*ret);
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_buffer(uint64_t nativeRef, uint64_t offset, void** ret) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  if (isEnabled(TRACE))
    log(TRACE, COMPONENT, "getting buffer and putting the results at 0x%P", ret);
  if (!ret)
    return fromErrno(EINVAL);
  return (uint64_t)((SharedMemory*)nativeRef)->getBuffer((std::size_t)offset, *ret);
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_lock(uint64_t nativeRef, int64_t millis, int32_t aggressive) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->lock(millis, aggressive == 0 ? false : true);
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_unlock(uint64_t nativeRef) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->unlock();
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_isMessageAvailable(uint64_t nativeRef, int32_t* ret, int32_t mailbox) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  if (!ret)
    return fromErrno(EINVAL);
  bool isAvail = false;
  auto aret = (uint64_t)((SharedMemory*)nativeRef)->isMessageAvailable(isAvail, (std::size_t)mailbox);
  *ret = isAvail ? 1 : 0;
  return aret;
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_canWriteMessage(uint64_t nativeRef, int32_t* ret, int32_t mailbox) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  if (!ret)
    return fromErrno(EINVAL);
  bool isAvail = false;
  auto aret = (uint64_t)((SharedMemory*)nativeRef)->canWriteMessage(isAvail, (std::size_t)mailbox);
  *ret = isAvail ? 1 : 0;
  return aret;
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_postMessage(uint64_t nativeRef, int32_t mailbox) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  uint64_t ret = ((SharedMemory*)nativeRef)->postMessage((std::size_t)mailbox);
  return ret;
}

KAI_EXPORT uint64_t pilecv4j_ipc_shmQueue_unpostMessage(uint64_t nativeRef, int32_t mailbox) {
  PCV4K_IPC_TRACE;
  NULL_CHECK(nativeRef);
  return ((SharedMemory*)nativeRef)->unpostMessage((std::size_t)mailbox);
}

}
}
}

