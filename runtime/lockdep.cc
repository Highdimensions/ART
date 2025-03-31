#include "art_method-inl.h"
#include "jni/java_vm_ext.h"
#include "mirror/class-alloc-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object_array.h"
#include "obj_ptr-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"

#ifdef ART_TARGET_ANDROID
#include <android-base/properties.h>
#endif

namespace art {

static bool IsDynamicLockdepEnabled() {
#ifdef ART_TARGET_ANDROID
  static bool result = android::base::GetBoolProperty("debug.art.lockdep", false);
  return result;
#else
  return false;
#endif
}

bool Runtime::IsLockdepEnabled() { return IsDynamicLockdepEnabled(); }

static bool ShouldRecordBacktrace() {
#ifdef ART_TARGET_ANDROID
  static bool result = android::base::GetBoolProperty("debug.art.lockdep.backtrace", false);
  return result;
#else
  return false;
#endif
}

static ObjPtr<mirror::Throwable> CollectBacktrace() REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Throwable> result;

  Thread* current_thread = Thread::Current();
  JNIEnv* env = current_thread->GetJniEnv();
  env->PushLocalFrame(128);

  {
    ScopedObjectAccessUnchecked soa(current_thread);

    ObjPtr<mirror::Class> throwable_class =
        soa.Decode<mirror::Class>(WellKnownClasses::java_lang_Throwable);
    ObjPtr<mirror::Throwable> throwable =
        ObjPtr<mirror::Throwable>::DownCast(throwable_class->AllocObject(current_thread));

    jobject backtrace_jobj = Thread::Current()->CreateInternalStackTrace(soa);
    if (backtrace_jobj != nullptr) {
      throwable->SetStackState(soa.Decode<mirror::Object>(backtrace_jobj).Ptr());
      result = throwable;
    }
  }

  env->PopLocalFrame(nullptr);
  return result;
}

void Runtime::VisitLockdepRoots(RootVisitor* visitor) {
  RootInfo info(kRootVMInternal);
  std::unordered_set<GcRoot<mirror::Throwable>*> roots;
  ReaderMutexLock outer_lock(Thread::Current(), lock_deps_mutex_);
  for (const auto& outer : lock_deps_) {
    LockDeps* deps = outer.second.get();
    ReaderMutexLock inner_lock(Thread::Current(), deps->mutex);
    for (const auto& inner : deps->deps) {
      roots.insert(&inner.second->backtrace);
    }
  }
  for (auto* root : roots) {
    root->VisitRoot(visitor, info);
  }
}

Runtime::LockDeps* Runtime::GetLockDeps(int32_t mutex_hashcode) {
  {
    ReaderMutexLock lock(Thread::Current(), lock_deps_mutex_);
    auto it = lock_deps_.find(mutex_hashcode);
    if (it != lock_deps_.end()) {
      return it->second.get();
    }
  }

  return nullptr;
}

Runtime::LockDeps* Runtime::CreateLockDeps(int32_t mutex_hashcode) {
  auto result = GetLockDeps(mutex_hashcode);
  if (result) {
    return result;
  }

  // We don't need to lock across the Get and insertion, because TrackObjectLocked is called after
  // the monitor itself is acquired, so CreateLockDeps for a given mutex is automatically
  // synchronized.
  result = new Runtime::LockDeps();
  WriterMutexLock lock(Thread::Current(), lock_deps_mutex_);
  lock_deps_.emplace(mutex_hashcode, result);
  return result;
}

void Runtime::ClearLockDeps() {
  WriterMutexLock lock(Thread::Current(), lock_deps_mutex_);
  lock_deps_.clear();
}

void Runtime::TrackObjectLocked(int32_t mutex_hashcode,
                                const std::vector<int32_t>& currently_locked) {
  std::unordered_map<uint32_t, ObjPtr<mirror::Throwable>> violations;
  std::vector<LockRecord*> new_records;

  if (IsLockdepEnabled()) {
    // Check for lock ordering violation.
    LockDeps* deps = GetLockDeps(mutex_hashcode);
    if (deps) {
      WriterMutexLock lock(Thread::Current(), deps->mutex);
      // Are we already holding any of the lock's dependencies?
      for (int32_t dep : currently_locked) {
        auto it = deps->deps.find(dep);
        if (it != deps->deps.end() && !it->second->warned) {
          it->second->warned = true;

          ObjPtr<mirror::Throwable> old_backtrace;
          if (ShouldRecordBacktrace()) {
            old_backtrace = it->second->backtrace.Read<kWithoutReadBarrier>();
          }
          violations.insert(std::make_pair(dep, old_backtrace));
        }
      }
    }
  }

  if (IsDynamicLockdepEnabled()) {
    // Record the dependencies of this mutex.

    for (int32_t previously_held : currently_locked) {
      // We need to lock/unlock manually because the RAII helpers are lock_guard, not unique_lock,
      // and we need to drop the lock to generate a backtrace.
      // TODO: Make this a reader lock instead of a writer lock?
      LockDeps* deps = CreateLockDeps(previously_held);

      WriterMutexLock lock(Thread::Current(), deps->mutex);

      auto it = deps->deps.find(mutex_hashcode);
      if (it != deps->deps.end()) {
        continue;
      }

      auto record = std::make_unique<LockRecord>();
      new_records.push_back(record.get());
      deps->deps.insert(std::make_pair(mutex_hashcode, std::move(record)));
    }
  }

  ObjPtr<mirror::Throwable> backtrace = nullptr;
  if (!(violations.empty() && new_records.empty())) {
    // We need a backtrace.
    backtrace = CollectBacktrace();
  }

  for (const auto& it : violations) {
    WellKnownClasses::dalvik_system_VMRuntime_lockOrderingViolated
        ->InvokeStatic<'V', 'I', 'I', 'L', 'L'>(
            Thread::Current(), mutex_hashcode, it.first, backtrace, it.second);
  }

  for (auto* record : new_records) {
    record->backtrace = GcRoot(backtrace);
  }
}

}  // namespace art
