// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequence_manager/tasks.h"

#include "base/task/sequence_manager/task_order.h"

namespace base {
namespace sequence_manager {

Task::Task(internal::PostedTask posted_task,
           EnqueueOrder sequence_order,
           EnqueueOrder enqueue_order,
           TimeTicks queue_time,
           WakeUpResolution resolution)
    : PendingTask(posted_task.location,
                  std::move(posted_task.callback),
                  queue_time,
                  absl::holds_alternative<base::TimeTicks>(
                      posted_task.delay_or_delayed_run_time)
                      ? absl::get<base::TimeTicks>(
                            posted_task.delay_or_delayed_run_time)
                      : base::TimeTicks()),
      nestable(posted_task.nestable),
      task_type(posted_task.task_type),
      task_runner(std::move(posted_task.task_runner)),
      enqueue_order_(enqueue_order) {
  DCHECK(!absl::holds_alternative<base::TimeDelta>(
             posted_task.delay_or_delayed_run_time) ||
         absl::get<base::TimeDelta>(posted_task.delay_or_delayed_run_time)
             .is_zero());
  // We use |sequence_num| when comparing PendingTask for ordering purposes
  // and it may wrap around to a negative number during the static cast, hence,
  // TaskQueueImpl::DelayedIncomingQueue is especially sensitive to a potential
  // change of |PendingTask::sequence_num|'s type.
  static_assert(std::is_same<decltype(sequence_num), int>::value, "");
  sequence_num = static_cast<int>(sequence_order);
  this->is_high_res = resolution == WakeUpResolution::kHigh;
}

Task::Task(Task&& move_from) = default;

Task::~Task() = default;

Task& Task::operator=(Task&& other) = default;

TaskOrder Task::task_order() const {
  return TaskOrder(enqueue_order(), delayed_run_time, sequence_num);
}

namespace internal {

PostedTask::PostedTask(scoped_refptr<SequencedTaskRunner> task_runner,
                       OnceClosure callback,
                       Location location,
                       TimeDelta delay,
                       Nestable nestable,
                       TaskType task_type)
    : callback(std::move(callback)),
      location(location),
      nestable(nestable),
      task_type(task_type),
      delay_or_delayed_run_time(delay),
      task_runner(std::move(task_runner)) {}

PostedTask::PostedTask(scoped_refptr<SequencedTaskRunner> task_runner,
                       OnceClosure callback,
                       Location location,
                       TimeTicks delayed_run_time,
                       Nestable nestable,
                       TaskType task_type)
    : callback(std::move(callback)),
      location(location),
      nestable(nestable),
      task_type(task_type),
      delay_or_delayed_run_time(delayed_run_time),
      task_runner(std::move(task_runner)) {}

PostedTask::PostedTask(PostedTask&& move_from) noexcept = default;
PostedTask::~PostedTask() = default;

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base
