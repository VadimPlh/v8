// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/futex-emulation.h"

#include <limits>

#include "src/api/api-inl.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/execution/isolate.h"
#include "src/execution/vm-state-inl.h"
#include "src/handles/handles-inl.h"
#include "src/numbers/conversions.h"
#include "src/objects/bigint.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

using AtomicsWaitEvent = v8::Isolate::AtomicsWaitEvent;

base::LazyMutex FutexEmulation::mutex_ = LAZY_MUTEX_INITIALIZER;
base::LazyInstance<FutexWaitList>::type FutexEmulation::wait_list_ =
    LAZY_INSTANCE_INITIALIZER;

FutexWaitListNode::~FutexWaitListNode() {
  // Assert that the timeout task was cancelled.
  DCHECK_EQ(CancelableTaskManager::kInvalidTaskId, timeout_task_id_);
}

bool FutexWaitListNode::CancelTimeoutTask() {
  if (timeout_task_id_ != CancelableTaskManager::kInvalidTaskId) {
    auto return_value = cancelable_task_manager_->TryAbort(timeout_task_id_);
    timeout_task_id_ = CancelableTaskManager::kInvalidTaskId;
    return return_value != TryAbortResult::kTaskRunning;
  }
  return true;
}

void FutexWaitListNode::NotifyWake() {
  DCHECK(!IsAsync());
  // Lock the FutexEmulation mutex before notifying. We know that the mutex
  // will have been unlocked if we are currently waiting on the condition
  // variable. The mutex will not be locked if FutexEmulation::Wait hasn't
  // locked it yet. In that case, we set the interrupted_
  // flag to true, which will be tested after the mutex locked by a future wait.
  base::MutexGuard lock_guard(FutexEmulation::mutex_.Pointer());
  // if not waiting, this will not have any effect.
  cond_.NotifyOne();
  interrupted_ = true;
}

class ResolveAsyncWaiterPromisesTask : public CancelableTask {
 public:
  ResolveAsyncWaiterPromisesTask(CancelableTaskManager* cancelable_task_manager,
                                 Isolate* isolate)
      : CancelableTask(cancelable_task_manager), isolate_(isolate) {}

  void RunInternal() override {
    FutexEmulation::ResolveAsyncWaiterPromises(isolate_);
  }

 private:
  Isolate* isolate_;
};

class AsyncWaiterTimeoutTask : public CancelableTask {
 public:
  AsyncWaiterTimeoutTask(CancelableTaskManager* cancelable_task_manager,
                         FutexWaitListNode* node)
      : CancelableTask(cancelable_task_manager), node_(node) {}

  void RunInternal() override {
    FutexEmulation::HandleAsyncWaiterTimeout(node_);
  }

 private:
  FutexWaitListNode* node_;
};

void FutexEmulation::NotifyAsyncWaiter(FutexWaitListNode* node) {
  // This function can run in any thread.

  FutexEmulation::mutex_.Pointer()->AssertHeld();

  // Nullify the timeout time; this distinguishes timed out waiters from
  // woken up ones.
  node->async_timeout_time_ = base::TimeTicks();
  // Try to cancel the timeout task. If cancelling fails, the task is already
  // running. In that case, it cannot proceed beyond waiting for the mutex,
  // since we're holding it. When it gets the mutex, it will see that waiting_
  // is false, and ignore the FutexWaitListNode.

  // Using the CancelableTaskManager here is OK since the Isolate is guaranteed
  // to be alive - FutexEmulation::IsolateDeinit removes all FutexWaitListNodes
  // owned by an Isolate which is going to die.
  node->CancelTimeoutTask();

  wait_list_.Pointer()->RemoveNode(node);

  // Schedule a task for resolving the Promise.
  auto& isolate_map = wait_list_.Pointer()->isolate_promises_to_resolve_;
  auto it = isolate_map.find(node->isolate_for_async_waiters_);
  if (it == isolate_map.end()) {
    // This Isolate doesn't have other Promises to resolve at the moment.
    isolate_map.insert(std::make_pair(node->isolate_for_async_waiters_,
                                      FutexWaitList::HeadAndTail{node, node}));
    auto task = std::make_unique<ResolveAsyncWaiterPromisesTask>(
        node->cancelable_task_manager_, node->isolate_for_async_waiters_);
    node->task_runner_->PostNonNestableTask(std::move(task));
  } else {
    // Add this Node into the existing list.
    node->prev_ = it->second.tail;
    it->second.tail->next_ = node;
    it->second.tail = node;
  }
}

void FutexWaitList::AddNode(FutexWaitListNode* node) {
  DCHECK_NULL(node->prev_);
  DCHECK_NULL(node->next_);
  if (tail_) {
    tail_->next_ = node;
  } else {
    head_ = node;
  }

  node->prev_ = tail_;
  tail_ = node;

  Verify();
}

void FutexWaitList::RemoveNode(FutexWaitListNode* node) {
  DCHECK(NodeIsOnList(node, head_));

  if (node->prev_) {
    node->prev_->next_ = node->next_;
  } else {
    DCHECK_EQ(node, head_);
    head_ = node->next_;
  }

  if (node->next_) {
    node->next_->prev_ = node->prev_;
  } else {
    DCHECK_EQ(node, tail_);
    tail_ = node->prev_;
  }

  node->prev_ = node->next_ = nullptr;

  Verify();
}

void AtomicsWaitWakeHandle::Wake() {
  // Adding a separate `NotifyWake()` variant that doesn't acquire the lock
  // itself would likely just add unnecessary complexity..
  // The split lock by itself isn’t an issue, as long as the caller properly
  // synchronizes this with the closing `AtomicsWaitCallback`.
  {
    base::MutexGuard lock_guard(FutexEmulation::mutex_.Pointer());
    stopped_ = true;
  }
  isolate_->futex_wait_list_node()->NotifyWake();
}

enum WaitReturnValue : int { kOk = 0, kNotEqual = 1, kTimedOut = 2 };

namespace {

Object WaitJsTranslateReturn(Isolate* isolate, Object res) {
  if (res.IsSmi()) {
    int val = Smi::ToInt(res);
    switch (val) {
      case WaitReturnValue::kOk:
        return ReadOnlyRoots(isolate).ok_string();
      case WaitReturnValue::kNotEqual:
        return ReadOnlyRoots(isolate).not_equal_string();
      case WaitReturnValue::kTimedOut:
        return ReadOnlyRoots(isolate).timed_out_string();
      default:
        UNREACHABLE();
    }
  }
  return res;
}

}  // namespace

Object FutexEmulation::WaitJs32(Isolate* isolate, WaitMode mode,
                                Handle<JSArrayBuffer> array_buffer, size_t addr,
                                int32_t value, double rel_timeout_ms) {
  Object res =
      Wait<int32_t>(isolate, mode, array_buffer, addr, value, rel_timeout_ms);
  return WaitJsTranslateReturn(isolate, res);
}

Object FutexEmulation::WaitJs64(Isolate* isolate, WaitMode mode,
                                Handle<JSArrayBuffer> array_buffer, size_t addr,
                                int64_t value, double rel_timeout_ms) {
  Object res =
      Wait<int64_t>(isolate, mode, array_buffer, addr, value, rel_timeout_ms);
  return WaitJsTranslateReturn(isolate, res);
}

Object FutexEmulation::WaitWasm32(Isolate* isolate,
                                  Handle<JSArrayBuffer> array_buffer,
                                  size_t addr, int32_t value,
                                  int64_t rel_timeout_ns) {
  return Wait<int32_t>(isolate, WaitMode::kSync, array_buffer, addr, value,
                       rel_timeout_ns >= 0, rel_timeout_ns);
}

Object FutexEmulation::WaitWasm64(Isolate* isolate,
                                  Handle<JSArrayBuffer> array_buffer,
                                  size_t addr, int64_t value,
                                  int64_t rel_timeout_ns) {
  return Wait<int64_t>(isolate, WaitMode::kSync, array_buffer, addr, value,
                       rel_timeout_ns >= 0, rel_timeout_ns);
}

template <typename T>
Object FutexEmulation::Wait(Isolate* isolate, WaitMode mode,
                            Handle<JSArrayBuffer> array_buffer, size_t addr,
                            T value, double rel_timeout_ms) {
  DCHECK_LT(addr, array_buffer->byte_length());

  bool use_timeout = rel_timeout_ms != V8_INFINITY;
  int64_t rel_timeout_ns = -1;

  if (use_timeout) {
    // Convert to nanoseconds.
    double timeout_ns = rel_timeout_ms *
                        base::Time::kNanosecondsPerMicrosecond *
                        base::Time::kMicrosecondsPerMillisecond;
    if (timeout_ns > static_cast<double>(std::numeric_limits<int64_t>::max())) {
      // 2**63 nanoseconds is 292 years. Let's just treat anything greater as
      // infinite.
      use_timeout = false;
    } else {
      rel_timeout_ns = static_cast<int64_t>(timeout_ns);
    }
  }
  return Wait(isolate, mode, array_buffer, addr, value, use_timeout,
              rel_timeout_ns);
}

namespace {
double WaitTimeoutInMs(double timeout_ns) {
  return timeout_ns < 0
             ? V8_INFINITY
             : timeout_ns / (base::Time::kNanosecondsPerMicrosecond *
                             base::Time::kMicrosecondsPerMillisecond);
}
}  // namespace

template <typename T>
Object FutexEmulation::Wait(Isolate* isolate, WaitMode mode,
                            Handle<JSArrayBuffer> array_buffer, size_t addr,
                            T value, bool use_timeout, int64_t rel_timeout_ns) {
  if (mode == WaitMode::kSync) {
    return WaitSync(isolate, array_buffer, addr, value, use_timeout,
                    rel_timeout_ns);
  }
  DCHECK_EQ(mode, WaitMode::kAsync);
  return WaitAsync(isolate, array_buffer, addr, value, use_timeout,
                   rel_timeout_ns);
}

template <typename T>
Object FutexEmulation::WaitSync(Isolate* isolate,
                                Handle<JSArrayBuffer> array_buffer, size_t addr,
                                T value, bool use_timeout,
                                int64_t rel_timeout_ns) {
  VMState<ATOMICS_WAIT> state(isolate);
  base::TimeDelta rel_timeout =
      base::TimeDelta::FromNanoseconds(rel_timeout_ns);

  // We have to convert the timeout back to double for the AtomicsWaitCallback.
  double rel_timeout_ms = WaitTimeoutInMs(static_cast<double>(rel_timeout_ns));
  AtomicsWaitWakeHandle stop_handle(isolate);

  isolate->RunAtomicsWaitCallback(AtomicsWaitEvent::kStartWait, array_buffer,
                                  addr, value, rel_timeout_ms, &stop_handle);

  if (isolate->has_scheduled_exception()) {
    return isolate->PromoteScheduledException();
  }

  Handle<Object> result;
  AtomicsWaitEvent callback_result = AtomicsWaitEvent::kWokenUp;

  do {  // Not really a loop, just makes it easier to break out early.
    base::MutexGuard lock_guard(mutex_.Pointer());
    std::shared_ptr<BackingStore> backing_store =
        array_buffer->GetBackingStore();
    DCHECK(backing_store);
    FutexWaitListNode* node = isolate->futex_wait_list_node();
    node->backing_store_ = backing_store;
    node->wait_addr_ = addr;
    node->waiting_ = true;

    // Reset node->waiting_ = false when leaving this scope (but while
    // still holding the lock).
    ResetWaitingOnScopeExit reset_waiting(node);

    std::atomic<T>* p = reinterpret_cast<std::atomic<T>*>(
        static_cast<int8_t*>(backing_store->buffer_start()) + addr);
    if (p->load() != value) {
      result = handle(Smi::FromInt(WaitReturnValue::kNotEqual), isolate);
      callback_result = AtomicsWaitEvent::kNotEqual;
      break;
    }

    base::TimeTicks timeout_time;
    base::TimeTicks current_time;

    if (use_timeout) {
      current_time = base::TimeTicks::Now();
      timeout_time = current_time + rel_timeout;
    }

    wait_list_.Pointer()->AddNode(node);

    while (true) {
      bool interrupted = node->interrupted_;
      node->interrupted_ = false;

      // Unlock the mutex here to prevent deadlock from lock ordering between
      // mutex_ and mutexes locked by HandleInterrupts.
      mutex_.Pointer()->Unlock();

      // Because the mutex is unlocked, we have to be careful about not dropping
      // an interrupt. The notification can happen in three different places:
      // 1) Before Wait is called: the notification will be dropped, but
      //    interrupted_ will be set to 1. This will be checked below.
      // 2) After interrupted has been checked here, but before mutex_ is
      //    acquired: interrupted is checked again below, with mutex_ locked.
      //    Because the wakeup signal also acquires mutex_, we know it will not
      //    be able to notify until mutex_ is released below, when waiting on
      //    the condition variable.
      // 3) After the mutex is released in the call to WaitFor(): this
      // notification will wake up the condition variable. node->waiting() will
      // be false, so we'll loop and then check interrupts.
      if (interrupted) {
        Object interrupt_object = isolate->stack_guard()->HandleInterrupts();
        if (interrupt_object.IsException(isolate)) {
          result = handle(interrupt_object, isolate);
          callback_result = AtomicsWaitEvent::kTerminatedExecution;
          mutex_.Pointer()->Lock();
          break;
        }
      }

      mutex_.Pointer()->Lock();

      if (node->interrupted_) {
        // An interrupt occurred while the mutex_ was unlocked. Don't wait yet.
        continue;
      }

      if (stop_handle.has_stopped()) {
        node->waiting_ = false;
        callback_result = AtomicsWaitEvent::kAPIStopped;
      }

      if (!node->waiting_) {
        result = handle(Smi::FromInt(WaitReturnValue::kOk), isolate);
        break;
      }

      // No interrupts, now wait.
      if (use_timeout) {
        current_time = base::TimeTicks::Now();
        if (current_time >= timeout_time) {
          result = handle(Smi::FromInt(WaitReturnValue::kTimedOut), isolate);
          callback_result = AtomicsWaitEvent::kTimedOut;
          break;
        }

        base::TimeDelta time_until_timeout = timeout_time - current_time;
        DCHECK_GE(time_until_timeout.InMicroseconds(), 0);
        bool wait_for_result =
            node->cond_.WaitFor(mutex_.Pointer(), time_until_timeout);
        USE(wait_for_result);
      } else {
        node->cond_.Wait(mutex_.Pointer());
      }

      // Spurious wakeup, interrupt or timeout.
    }

    wait_list_.Pointer()->RemoveNode(node);
  } while (false);

  isolate->RunAtomicsWaitCallback(callback_result, array_buffer, addr, value,
                                  rel_timeout_ms, nullptr);

  if (isolate->has_scheduled_exception()) {
    CHECK_NE(callback_result, AtomicsWaitEvent::kTerminatedExecution);
    result = handle(isolate->PromoteScheduledException(), isolate);
  }

  return *result;
}

FutexWaitListNode::FutexWaitListNode(
    const std::shared_ptr<BackingStore>& backing_store, size_t wait_addr,
    Handle<JSObject> promise, Isolate* isolate)
    : isolate_for_async_waiters_(isolate),
      backing_store_(backing_store),
      wait_addr_(wait_addr),
      waiting_(true) {
  auto v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  task_runner_ = V8::GetCurrentPlatform()->GetForegroundTaskRunner(v8_isolate);
  cancelable_task_manager_ = isolate->cancelable_task_manager();

  v8::Local<v8::Promise> local_promise = Utils::PromiseToLocal(promise);
  promise_.Reset(v8_isolate, local_promise);
  promise_.SetWeak();
  Handle<NativeContext> native_context(isolate->native_context());
  v8::Local<v8::Context> local_native_context =
      Utils::ToLocal(Handle<Context>::cast(native_context));
  native_context_.Reset(v8_isolate, local_native_context);
  native_context_.SetWeak();

  // Add the Promise into the NativeContext's atomics_waitasync_promises set, so
  // that the list keeps it alive.
  Handle<OrderedHashSet> promises(native_context->atomics_waitasync_promises(),
                                  isolate);
  promises = OrderedHashSet::Add(isolate, promises, promise).ToHandleChecked();
  native_context->set_atomics_waitasync_promises(*promises);
}

template <typename T>
Object FutexEmulation::WaitAsync(Isolate* isolate,
                                 Handle<JSArrayBuffer> array_buffer,
                                 size_t addr, T value, bool use_timeout,
                                 int64_t rel_timeout_ns) {
  DCHECK(FLAG_harmony_atomics_waitasync);
  base::TimeDelta rel_timeout =
      base::TimeDelta::FromNanoseconds(rel_timeout_ns);

  Factory* factory = isolate->factory();
  Handle<JSObject> result = factory->NewJSObject(isolate->object_function());

  std::shared_ptr<BackingStore> backing_store = array_buffer->GetBackingStore();

  // 17. Let w be ! AtomicLoad(typedArray, i).
  std::atomic<T>* p = reinterpret_cast<std::atomic<T>*>(
      static_cast<int8_t*>(backing_store->buffer_start()) + addr);
  if (p->load() != value) {
    // 18. If v is not equal to w, then
    //   a. Perform LeaveCriticalSection(WL).
    //   ...
    //   c. Perform ! CreateDataPropertyOrThrow(resultObject, "async", false).
    //   d. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
    //     "not-equal").
    //   e. Return resultObject.
    CHECK(
        JSReceiver::CreateDataProperty(isolate, result, factory->async_string(),
                                       factory->false_value(), Just(kDontThrow))
            .FromJust());
    CHECK(JSReceiver::CreateDataProperty(
              isolate, result, factory->value_string(),
              factory->not_equal_string(), Just(kDontThrow))
              .FromJust());
    return *result;
  }

  if (use_timeout && rel_timeout_ns == 0) {
    // 19. If t is 0 and mode is async, then
    //   ...
    //   b. Perform LeaveCriticalSection(WL).
    //   c. Perform ! CreateDataPropertyOrThrow(resultObject, "async", false).
    //   d. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
    //     "timed-out").
    //   e. Return resultObject.
    CHECK(
        JSReceiver::CreateDataProperty(isolate, result, factory->async_string(),
                                       factory->false_value(), Just(kDontThrow))
            .FromJust());
    CHECK(JSReceiver::CreateDataProperty(
              isolate, result, factory->value_string(),
              factory->timed_out_string(), Just(kDontThrow))
              .FromJust());
    return *result;
  }

  Handle<JSObject> promise_capability = factory->NewJSPromise();
  FutexWaitListNode* node =
      new FutexWaitListNode(backing_store, addr, promise_capability, isolate);

  {
    base::MutexGuard lock_guard(mutex_.Pointer());
    wait_list_.Pointer()->AddNode(node);
  }
  if (use_timeout) {
    node->async_timeout_time_ = base::TimeTicks::Now() + rel_timeout;
    auto task = std::make_unique<AsyncWaiterTimeoutTask>(
        node->cancelable_task_manager_, node);
    node->timeout_task_id_ = task->id();
    node->task_runner_->PostNonNestableDelayedTask(std::move(task),
                                                   rel_timeout.InSecondsF());
  }

  // 26. Perform ! CreateDataPropertyOrThrow(resultObject, "async", true).
  // 27. Perform ! CreateDataPropertyOrThrow(resultObject, "value",
  // promiseCapability.[[Promise]]).
  // 28. Return resultObject.
  CHECK(JSReceiver::CreateDataProperty(isolate, result, factory->async_string(),
                                       factory->true_value(), Just(kDontThrow))
            .FromJust());
  CHECK(JSReceiver::CreateDataProperty(isolate, result, factory->value_string(),
                                       promise_capability, Just(kDontThrow))
            .FromJust());
  return *result;
}

Object FutexEmulation::Wake(Handle<JSArrayBuffer> array_buffer, size_t addr,
                            uint32_t num_waiters_to_wake) {
  DCHECK_LT(addr, array_buffer->byte_length());

  int waiters_woken = 0;
  std::shared_ptr<BackingStore> backing_store = array_buffer->GetBackingStore();

  base::MutexGuard lock_guard(mutex_.Pointer());
  FutexWaitListNode* node = wait_list_.Pointer()->head_;
  while (node && num_waiters_to_wake > 0) {
    bool delete_this_node = false;
    std::shared_ptr<BackingStore> node_backing_store =
        node->backing_store_.lock();

    if (!node->waiting_) {
      node = node->next_;
      continue;
    }
    if (backing_store.get() == node_backing_store.get() &&
        addr == node->wait_addr_) {
      node->waiting_ = false;

      // Retrieve the next node to iterate before calling NotifyAsyncWaiter,
      // since NotifyAsyncWaiter will take the node out of the linked list.
      auto old_node = node;
      node = node->next_;
      if (old_node->IsAsync()) {
        NotifyAsyncWaiter(old_node);
      } else {
        old_node->cond_.NotifyOne();
      }
      if (num_waiters_to_wake != kWakeAll) {
        --num_waiters_to_wake;
      }
      waiters_woken++;
      continue;
    }
    if (node_backing_store.get() == nullptr &&
        node->async_timeout_time_ == base::TimeTicks()) {
      // Backing store has been deleted and the node is still waiting, and
      // there's no timeout. It's never going to be woken up, so we can clean
      // it up now. We don't need to cancel the timeout task, because there is
      // none.
      DCHECK(node->IsAsync());
      DCHECK_EQ(CancelableTaskManager::kInvalidTaskId, node->timeout_task_id_);
      delete_this_node = true;
    } else if (node->IsAsync() && node->native_context_.IsEmpty()) {
      // The NativeContext related to the async waiter has been deleted.
      // Ditto, clean up now.

      // Using the CancelableTaskManager here is OK since the Isolate is
      // guaranteed to be alive - FutexEmulation::IsolateDeinit removes all
      // FutexWaitListNodes owned by an Isolate which is going to die.
      if (node->CancelTimeoutTask()) {
        delete_this_node = true;
      }
      // If cancelling the timeout task failed, the timeout task is already
      // running and will clean up the node.
    }

    if (delete_this_node) {
      auto old_node = node;
      node = node->next_;
      wait_list_.Pointer()->RemoveNode(old_node);
      delete old_node;
    } else {
      node = node->next_;
    }
  }

  return Smi::FromInt(waiters_woken);
}

void FutexEmulation::CleanupAsyncWaiterPromise(FutexWaitListNode* node) {
  DCHECK(FLAG_harmony_atomics_waitasync);
  DCHECK(node->IsAsync());

  Isolate* isolate = node->isolate_for_async_waiters_;
  auto v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);

  // This function must run in the main thread of node's Isolate.
  DCHECK_EQ(isolate->thread_id(), ThreadId::Current());

  if (!node->promise_.IsEmpty()) {
    Handle<JSPromise> promise = Handle<JSPromise>::cast(
        Utils::OpenHandle(*node->promise_.Get(v8_isolate)));
    // Promise keeps the NativeContext alive.
    DCHECK(!node->native_context_.IsEmpty());
    Handle<NativeContext> native_context = Handle<NativeContext>::cast(
        Utils::OpenHandle(*node->native_context_.Get(v8_isolate)));

    // Remove the Promise from the NativeContext's set.
    Handle<OrderedHashSet> promises(
        native_context->atomics_waitasync_promises(), isolate);
    bool was_deleted = OrderedHashSet::Delete(isolate, *promises, *promise);
    DCHECK(was_deleted);
    USE(was_deleted);
    promises = OrderedHashSet::Shrink(isolate, promises);
    native_context->set_atomics_waitasync_promises(*promises);
  } else {
    // NativeContext keeps the Promise alive; if the Promise is dead then
    // surely NativeContext is too.
    DCHECK(node->native_context_.IsEmpty());
  }
}

FutexWaitListNode* FutexEmulation::DeleteAsyncWaiterNode(
    FutexWaitListNode* node) {
  auto next = node->next_;
  delete node;
  return next;
}

void FutexEmulation::ResolveAsyncWaiterPromise(FutexWaitListNode* node) {
  DCHECK(FLAG_harmony_atomics_waitasync);

  // This function must run in the main thread of node's Isolate.
  DCHECK_EQ(node->isolate_for_async_waiters_->thread_id(), ThreadId::Current());

  auto v8_isolate =
      reinterpret_cast<v8::Isolate*>(node->isolate_for_async_waiters_);

  if (!node->promise_.IsEmpty()) {
    Handle<JSPromise> promise = Handle<JSPromise>::cast(
        Utils::OpenHandle(*node->promise_.Get(v8_isolate)));
    Handle<String> result_string;
    // When waiters are notified, their async_timeout_time_ is reset. Having a
    // non-zero async_timeout_time_ here means the waiter timed out.
    if (node->async_timeout_time_ != base::TimeTicks()) {
      DCHECK(node->waiting_);
      result_string =
          node->isolate_for_async_waiters_->factory()->timed_out_string();
    } else {
      DCHECK(!node->waiting_);
      result_string = node->isolate_for_async_waiters_->factory()->ok_string();
    }
    MaybeHandle<Object> resolve_result =
        JSPromise::Resolve(promise, result_string);
    DCHECK(!resolve_result.is_null());
    USE(resolve_result);
  }
}

void FutexEmulation::ResolveAsyncWaiterPromises(Isolate* isolate) {
  DCHECK(FLAG_harmony_atomics_waitasync);

  // This function must run in the main thread of isolate.
  DCHECK_EQ(isolate->thread_id(), ThreadId::Current());

  base::MutexGuard lock_guard(mutex_.Pointer());
  FutexWaitListNode* node;
  {
    auto& isolate_map = wait_list_.Pointer()->isolate_promises_to_resolve_;
    auto it = isolate_map.find(isolate);
    DCHECK_NE(isolate_map.end(), it);

    node = it->second.head;
    isolate_map.erase(it);
  }

  HandleScope handle_scope(isolate);
  while (node) {
    DCHECK_EQ(isolate, node->isolate_for_async_waiters_);
    DCHECK(!node->waiting_);
    ResolveAsyncWaiterPromise(node);
    CleanupAsyncWaiterPromise(node);
    // We've already tried to cancel the timeout task for the node; since we're
    // now in the same thread the timeout task is supposed to run, we know the
    // timeout task will never happen, and it's safe to delete the node here.
    DCHECK_EQ(CancelableTaskManager::kInvalidTaskId, node->timeout_task_id_);
    node = DeleteAsyncWaiterNode(node);
  }
}

void FutexEmulation::HandleAsyncWaiterTimeout(FutexWaitListNode* node) {
  DCHECK(FLAG_harmony_atomics_waitasync);
  DCHECK(node->IsAsync());

  // This function must run in the main thread of node's Isolate.
  DCHECK_EQ(node->isolate_for_async_waiters_->thread_id(), ThreadId::Current());

  base::MutexGuard lock_guard(mutex_.Pointer());

  if (!node->waiting_) {
    // If the Node is not waiting, it's already scheduled to have its Promise
    // resolved. Ignore the timeout.
    return;
  }
  node->timeout_task_id_ = CancelableTaskManager::kInvalidTaskId;
  wait_list_.Pointer()->RemoveNode(node);
  HandleScope handle_scope(node->isolate_for_async_waiters_);
  ResolveAsyncWaiterPromise(node);
  CleanupAsyncWaiterPromise(node);
  delete node;
}

void FutexEmulation::IsolateDeinit(Isolate* isolate) {
  base::MutexGuard lock_guard(mutex_.Pointer());

  FutexWaitListNode* node = wait_list_.Pointer()->head_;
  while (node) {
    if (node->isolate_for_async_waiters_ == isolate) {
      // The Isolate is going away; don't bother cleaning up the Promises in the
      // NativeContext. Also we don't need to cancel the timeout task, since it
      // will be cancelled by Isolate::Deinit.
      node->timeout_task_id_ = CancelableTaskManager::kInvalidTaskId;
      wait_list_.Pointer()->RemoveNode(node);
      node = DeleteAsyncWaiterNode(node);
    } else {
      node = node->next_;
    }
  }

  auto& isolate_map = wait_list_.Pointer()->isolate_promises_to_resolve_;
  auto it = isolate_map.find(isolate);
  if (it != isolate_map.end()) {
    node = it->second.head;
    while (node) {
      DCHECK_EQ(isolate, node->isolate_for_async_waiters_);
      node = DeleteAsyncWaiterNode(node);
    }
    isolate_map.erase(it);
  }

  wait_list_.Pointer()->Verify();
}

Object FutexEmulation::NumWaitersForTesting(Handle<JSArrayBuffer> array_buffer,
                                            size_t addr) {
  DCHECK_LT(addr, array_buffer->byte_length());
  std::shared_ptr<BackingStore> backing_store = array_buffer->GetBackingStore();

  base::MutexGuard lock_guard(mutex_.Pointer());

  int waiters = 0;
  FutexWaitListNode* node = wait_list_.Pointer()->head_;
  while (node) {
    std::shared_ptr<BackingStore> node_backing_store =
        node->backing_store_.lock();
    if (backing_store.get() == node_backing_store.get() &&
        addr == node->wait_addr_ && node->waiting_) {
      waiters++;
    }

    node = node->next_;
  }

  return Smi::FromInt(waiters);
}

Object FutexEmulation::NumAsyncWaitersForTesting(Isolate* isolate) {
  base::MutexGuard lock_guard(mutex_.Pointer());

  int waiters = 0;
  FutexWaitListNode* node = wait_list_.Pointer()->head_;
  while (node) {
    if (node->isolate_for_async_waiters_ == isolate && node->waiting_) {
      waiters++;
    }
    node = node->next_;
  }

  return Smi::FromInt(waiters);
}

Object FutexEmulation::NumUnresolvedAsyncPromisesForTesting(
    Handle<JSArrayBuffer> array_buffer, size_t addr) {
  DCHECK_LT(addr, array_buffer->byte_length());
  std::shared_ptr<BackingStore> backing_store = array_buffer->GetBackingStore();

  base::MutexGuard lock_guard(mutex_.Pointer());

  int waiters = 0;

  auto& isolate_map = wait_list_.Pointer()->isolate_promises_to_resolve_;
  for (auto it : isolate_map) {
    FutexWaitListNode* node = it.second.head;
    while (node) {
      std::shared_ptr<BackingStore> node_backing_store =
          node->backing_store_.lock();
      if (backing_store.get() == node_backing_store.get() &&
          addr == node->wait_addr_ && !node->waiting_) {
        waiters++;
      }

      node = node->next_;
    }
  }

  return Smi::FromInt(waiters);
}

void FutexWaitList::VerifyNode(FutexWaitListNode* node, FutexWaitListNode* head,
                               FutexWaitListNode* tail) {
#ifdef DEBUG
  if (node->next_) {
    DCHECK_NE(node, tail);
    DCHECK_EQ(node, node->next_->prev_);
  } else {
    DCHECK_EQ(node, tail);
  }
  if (node->prev_) {
    DCHECK_NE(node, head);
    DCHECK_EQ(node, node->prev_->next_);
  } else {
    DCHECK_EQ(node, head);
  }

  if (node->async_timeout_time_ != base::TimeTicks()) {
    DCHECK(FLAG_harmony_atomics_waitasync);
    DCHECK(node->IsAsync());
  }

  DCHECK(NodeIsOnList(node, head));
#endif  // DEBUG
}

void FutexWaitList::Verify() {
#ifdef DEBUG
  FutexWaitListNode* node = head_;
  while (node) {
    VerifyNode(node, head_, tail_);
    node = node->next_;
  }

  for (auto it : isolate_promises_to_resolve_) {
    auto node = it.second.head;
    while (node) {
      VerifyNode(node, it.second.head, it.second.tail);
      DCHECK_EQ(it.first, node->isolate_for_async_waiters_);
      node = node->next_;
    }
  }
#endif  // DEBUG
}

bool FutexWaitList::NodeIsOnList(FutexWaitListNode* node,
                                 FutexWaitListNode* head) {
  auto n = head;
  while (n != nullptr) {
    if (n == node) {
      return true;
    }
    n = n->next_;
  }
  return false;
}

}  // namespace internal
}  // namespace v8