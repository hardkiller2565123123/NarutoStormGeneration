/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/string_utils.h>  // For TranslateAnsiStringAddress
#include <rex/system/xobject.h>
// #include <rex/kernel/xboxkrnl/private.h>  // TODO: JIT only
#include <rex/system/xenumerator.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xmodule.h>
#include <rex/system/xmutant.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xsemaphore.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(kernel_wait_trace, false, "Kernel",
                    "Trace potentially blocking kernel object waits")
    .debug_only();

namespace rex::system {

namespace {
std::atomic<uint64_t> g_wait_trace_sequence{0};
std::mutex g_wait_trace_mutex;
std::unordered_map<uint32_t, uint32_t> g_wait_trace_handle_counts;

struct WaitTraceContext {
  uint32_t thread_id = 0;
  uint32_t lr = 0;
  uint32_t ctr = 0;
  uint32_t r1 = 0;
  uint32_t r3 = 0;
};

WaitTraceContext CaptureWaitTraceContext() {
  WaitTraceContext result;
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (!thread_state || !thread_state->context()) {
    return result;
  }

  auto* ctx = thread_state->context();
  result.thread_id = thread_state->thread_id();
  result.lr = static_cast<uint32_t>(ctx->lr);
  result.ctr = ctx->ctr.u32;
  result.r1 = ctx->r1.u32;
  result.r3 = ctx->r3.u32;
  return result;
}

const char* ObjectTypeName(XObject::Type type) {
  switch (type) {
    case XObject::Type::Enumerator:
      return "Enumerator";
    case XObject::Type::Event:
      return "Event";
    case XObject::Type::File:
      return "File";
    case XObject::Type::IOCompletion:
      return "IOCompletion";
    case XObject::Type::Module:
      return "Module";
    case XObject::Type::Mutant:
      return "Mutant";
    case XObject::Type::NotifyListener:
      return "NotifyListener";
    case XObject::Type::Semaphore:
      return "Semaphore";
    case XObject::Type::Session:
      return "Session";
    case XObject::Type::Socket:
      return "Socket";
    case XObject::Type::SymbolicLink:
      return "SymbolicLink";
    case XObject::Type::Thread:
      return "Thread";
    case XObject::Type::Timer:
      return "Timer";
    case XObject::Type::Undefined:
    default:
      return "Undefined";
  }
}

bool ShouldTraceWait(uint64_t* opt_timeout) {
  return REXCVAR_GET(kernel_wait_trace) && (!opt_timeout || *opt_timeout != 0);
}

bool ShouldLogWaitTrace(uint64_t sequence, uint32_t handle) {
  if (sequence <= 64 || (sequence % 5000) == 0) {
    return true;
  }

  std::scoped_lock lock(g_wait_trace_mutex);
  uint32_t& count = g_wait_trace_handle_counts[handle];
  ++count;
  return count <= 4;
}

uint32_t ObjectHandleOrZero(XObject* object) {
  if (!object || object->handles().empty()) {
    return 0;
  }
  return object->handles()[0];
}

uint32_t ObjectGuestOrZero(XObject* object) {
  return object ? object->guest_object() : 0;
}

uint64_t TimeoutTraceValue(uint64_t* opt_timeout) {
  return opt_timeout ? *opt_timeout : UINT64_MAX;
}
}  // namespace

XObject::XObject(Type type) : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  // TODO: these are being asserted true... find out why
  // assert_true(handles_.empty());
  // assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t ptr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(ptr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(ptr);
  }
}

rex::Runtime* XObject::emulator() const {
  return kernel_state_->emulator_;
}
KernelState* XObject::kernel_state() const {
  return kernel_state_;
}
rex::memory::Memory* XObject::memory() const {
  return kernel_state_->memory();
}

XObject::Type XObject::type() const {
  return type_;
}

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) == X_STATUS_SUCCESS;
}

void XObject::Retain() {
  ++pointer_ref_count_;
}

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(stream::ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(stream::ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     stream::ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(),
      memory::load_and_swap<uint32_t>(memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    return 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                       uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandle();
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  const bool trace = ShouldTraceWait(opt_timeout);
  const uint64_t sequence = trace ? ++g_wait_trace_sequence : 0;
  const bool log_trace = trace && ShouldLogWaitTrace(sequence, ObjectHandleOrZero(this));
  if (log_trace) {
    const auto guest_ctx = CaptureWaitTraceContext();
    REXSYS_INFO(
        "[WAITTRACE] ENTER single seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} handle={:08X} type={} guest={:08X} reason={} mode={} alertable={} timeout={:016X}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        ObjectHandleOrZero(this), ObjectTypeName(type_), ObjectGuestOrZero(this), wait_reason,
        processor_mode, alertable, TimeoutTraceValue(opt_timeout));
  }

  auto result = rex::thread::Wait(wait_handle, alertable ? true : false, timeout_ms);
  X_STATUS status = X_STATUS_SUCCESS;
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      WaitCallback();
      status = X_STATUS_SUCCESS;
      break;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      status = X_STATUS_USER_APC;
      break;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      status = X_STATUS_TIMEOUT;
      break;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      status = X_STATUS_ABANDONED_WAIT_0;
      break;
  }
  if (log_trace) {
    REXSYS_INFO("[WAITTRACE] EXIT  single seq={} result={:08X}", sequence,
                static_cast<uint32_t>(status));
  }
  return status;
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object, uint32_t wait_reason,
                                uint32_t processor_mode, uint32_t alertable,
                                uint64_t* opt_timeout) {
  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  const bool trace = ShouldTraceWait(opt_timeout);
  const uint64_t sequence = trace ? ++g_wait_trace_sequence : 0;
  const bool log_trace = trace && ShouldLogWaitTrace(sequence, ObjectHandleOrZero(wait_object));
  if (log_trace) {
    const auto guest_ctx = CaptureWaitTraceContext();
    REXSYS_INFO(
        "[WAITTRACE] ENTER signal-wait seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} signal={:08X}/{} wait={:08X}/{} reason={} mode={} alertable={} timeout={:016X}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        ObjectHandleOrZero(signal_object), ObjectTypeName(signal_object->type()),
        ObjectHandleOrZero(wait_object), ObjectTypeName(wait_object->type()), wait_reason,
        processor_mode, alertable, TimeoutTraceValue(opt_timeout));
  }

  auto result =
      rex::thread::SignalAndWait(signal_object->GetWaitHandle(), wait_object->GetWaitHandle(),
                                 alertable ? true : false, timeout_ms);
  X_STATUS status = X_STATUS_SUCCESS;
  switch (result) {
    case rex::thread::WaitResult::kSuccess:
      wait_object->WaitCallback();
      status = X_STATUS_SUCCESS;
      break;
    case rex::thread::WaitResult::kUserCallback:
      // Or X_STATUS_ALERTED?
      status = X_STATUS_USER_APC;
      break;
    case rex::thread::WaitResult::kTimeout:
      rex::thread::MaybeYield();
      status = X_STATUS_TIMEOUT;
      break;
    default:
    case rex::thread::WaitResult::kAbandoned:
    case rex::thread::WaitResult::kFailed:
      status = X_STATUS_ABANDONED_WAIT_0;
      break;
  }
  if (log_trace) {
    REXSYS_INFO("[WAITTRACE] EXIT  signal-wait seq={} result={:08X}", sequence,
                static_cast<uint32_t>(status));
  }
  return status;
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects, uint32_t wait_type,
                               uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  std::vector<rex::thread::WaitHandle*> wait_handles(count);
  for (size_t i = 0; i < count; ++i) {
    wait_handles[i] = objects[i]->GetWaitHandle();
    assert_not_null(wait_handles[i]);
  }

  auto timeout_ms = opt_timeout ? std::chrono::milliseconds(chrono::Clock::ScaleGuestDurationMillis(
                                      TimeoutTicksToMs(*opt_timeout)))
                                : std::chrono::milliseconds::max();

  const bool trace = ShouldTraceWait(opt_timeout);
  const uint64_t sequence = trace ? ++g_wait_trace_sequence : 0;
  const bool log_trace =
      trace && ShouldLogWaitTrace(sequence, count ? ObjectHandleOrZero(objects[0]) : 0);
  if (log_trace) {
    const auto guest_ctx = CaptureWaitTraceContext();
    REXSYS_INFO(
        "[WAITTRACE] ENTER multi seq={} thid={} lr={:08X} ctr={:08X} r1={:08X} r3={:08X} count={} wait_type={} first={:08X}/{} reason={} mode={} alertable={} timeout={:016X}",
        sequence, guest_ctx.thread_id, guest_ctx.lr, guest_ctx.ctr, guest_ctx.r1, guest_ctx.r3,
        count, wait_type, count ? ObjectHandleOrZero(objects[0]) : 0,
        count ? ObjectTypeName(objects[0]->type()) : "None", wait_reason, processor_mode,
        alertable, TimeoutTraceValue(opt_timeout));
  }

  X_STATUS status = X_STATUS_SUCCESS;
  if (wait_type) {
    auto result =
        rex::thread::WaitAny(std::move(wait_handles), alertable ? true : false, timeout_ms);
    switch (result.first) {
      case rex::thread::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();

        status = X_STATUS(result.second);
        break;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        status = X_STATUS_USER_APC;
        break;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      default:
      case rex::thread::WaitResult::kAbandoned:
        status = X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        break;
      case rex::thread::WaitResult::kFailed:
        status = X_STATUS_UNSUCCESSFUL;
        break;
    }
  } else {
    auto result =
        rex::thread::WaitAll(std::move(wait_handles), alertable ? true : false, timeout_ms);
    switch (result) {
      case rex::thread::WaitResult::kSuccess:
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
        }

        status = X_STATUS_SUCCESS;
        break;
      case rex::thread::WaitResult::kUserCallback:
        // Or X_STATUS_ALERTED?
        status = X_STATUS_USER_APC;
        break;
      case rex::thread::WaitResult::kTimeout:
        rex::thread::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      default:
      case rex::thread::WaitResult::kAbandoned:
      case rex::thread::WaitResult::kFailed:
        status = X_STATUS_ABANDONED_WAIT_0;
        break;
    }
  }
  if (log_trace) {
    REXSYS_INFO("[WAITTRACE] EXIT  multi seq={} result={:08X}", sequence,
                static_cast<uint32_t>(status));
  }
  return status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  uint32_t total_size = size + sizeof(X_OBJECT_HEADER);

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  SetNativePointer(mem + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(mem);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header = kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list_blink & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state, void* native_ptr,
                                             int32_t as_type) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list_flink to a magic value. When set,
  // wait_list_blink will hold a handle to our object.

  auto global_lock = rex::thread::global_critical_region::AcquireDirect();

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);

  if (as_type == -1) {
    as_type = header->type;
  }

  if (header->wait_list_flink == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list_blink;
    auto object = kernel_state->object_table()->LookupObject<XObject>(handle);

    // TODO(benvanik): assert nothing has been changed in the struct.
    return object;
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    XObject* object = nullptr;
    switch (as_type) {
      case 0:  // EventNotificationObject
      case 1:  // EventSynchronizationObject
      {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        object = ev;
      } break;
      case 2:  // MutantObject
      {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        object = mutant;
      } break;
      case 5:  // SemaphoreObject
      {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        object = sem;
      } break;
      case 3:   // ProcessObject
      case 4:   // QueueObject
      case 6:   // ThreadObject
      case 7:   // GateObject
      case 8:   // TimerNotificationObject
      case 9:   // TimerSynchronizationObject
      case 18:  // ApcObject
      case 19:  // DpcObject
      case 20:  // DeviceQueueObject
      case 21:  // EventPairObject
      case 22:  // InterruptObject
      case 23:  // ProfileObject
      case 24:  // ThreadedDpcObject
      default:
        assert_always();
        return NULL;
    }

    // Stash pointer in struct.
    // FIXME: This assumes the object contains a dispatch header (some don't!)
    StashHandle(header, object->handle());

    return object_ref<XObject>(object);
  }
}

}  // namespace rex::system
