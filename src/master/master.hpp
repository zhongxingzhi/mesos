/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MASTER_HPP__
#define __MASTER_HPP__

#include <stdint.h>

#include <list>
#include <string>
#include <vector>

#include <boost/circular_buffer.hpp>

#include <mesos/resources.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timer.hpp>

#include <stout/cache.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/memory.hpp>
#include <stout/multihashmap.hpp>
#include <stout/option.hpp>

#include "common/protobuf_utils.hpp"
#include "common/type_utils.hpp"

#include "files/files.hpp"

#include "master/constants.hpp"
#include "master/contender.hpp"
#include "master/detector.hpp"
#include "master/flags.hpp"
#include "master/metrics.hpp"
#include "master/registrar.hpp"

#include "messages/messages.hpp"

namespace process {
class RateLimiter; // Forward declaration.
}

namespace mesos {
namespace internal {

// Forward declarations.
namespace registry {
class Slaves;
}

class Authenticator;
class Authorizer;
class WhitelistWatcher;

namespace master {

// Forward declarations.
namespace allocator {
class Allocator;
}

class Repairer;
class SlaveObserver;

struct Framework;
struct OfferValidator;
struct Role;
struct Slave;

class Master : public ProtobufProcess<Master>
{
public:
  Master(allocator::Allocator* allocator,
         Registrar* registrar,
         Repairer* repairer,
         Files* files,
         MasterContender* contender,
         MasterDetector* detector,
         const Option<Authorizer*>& authorizer,
         const Flags& flags = Flags());

  virtual ~Master();

  void submitScheduler(
      const std::string& name);
  void registerFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo);
  void reregisterFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool failover);
  void unregisterFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void deactivateFramework(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void resourceRequest(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<Request>& requests);
  void launchTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskInfo>& tasks,
      const Filters& filters,
      const std::vector<OfferID>& offerIds);
  void reviveOffers(
      const process::UPID& from,
      const FrameworkID& frameworkId);
  void killTask(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const TaskID& taskId);

  void statusUpdateAcknowledgement(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const TaskID& taskId,
      const std::string& uuid);

  void schedulerMessage(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      const std::string& data);
  void registerSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::string& version);
  void reregisterSlave(
      const process::UPID& from,
      const SlaveInfo& slaveInfo,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version);

  void unregisterSlave(
      const process::UPID& from,
      const SlaveID& slaveId);

  void statusUpdate(
      const StatusUpdate& update,
      const process::UPID& pid);

  void exitedExecutor(
      const process::UPID& from,
      const SlaveID& slaveId,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId,
      int32_t status);

  void shutdownSlave(
      const SlaveID& slaveId,
      const std::string& message);

  // TODO(bmahler): It would be preferred to use a unique libprocess
  // Process identifier (PID is not sufficient) for identifying the
  // framework instance, rather than relying on re-registration time.
  void frameworkFailoverTimeout(
      const FrameworkID& frameworkId,
      const process::Time& reregisteredTime);

  void offer(
      const FrameworkID& framework,
      const hashmap<SlaveID, Resources>& resources);

  void reconcileTasks(
      const process::UPID& from,
      const FrameworkID& frameworkId,
      const std::vector<TaskStatus>& statuses);

  void authenticate(
      const process::UPID& from,
      const process::UPID& pid);

  // Invoked when there is a newly elected leading master.
  // Made public for testing purposes.
  void detected(const process::Future<Option<MasterInfo>>& pid);

  // Invoked when the contender has lost the candidacy.
  // Made public for testing purposes.
  void lostCandidacy(const process::Future<Nothing>& lost);

  // Continuation of recover().
  // Made public for testing purposes.
  process::Future<Nothing> _recover(const Registry& registry);

  // Continuation of reregisterSlave().
  // Made public for testing purposes.
  // TODO(vinod): Instead of doing this create and use a
  // MockRegistrar.
  // TODO(dhamon): Consider FRIEND_TEST macro from gtest.
  void _reregisterSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::vector<ExecutorInfo>& executorInfos,
      const std::vector<Task>& tasks,
      const std::vector<Archive::Framework>& completedFrameworks,
      const std::string& version,
      const process::Future<bool>& readmit);

  MasterInfo info() const
  {
    return info_;
  }

protected:
  virtual void initialize();
  virtual void finalize();
  virtual void exited(const process::UPID& pid);
  virtual void visit(const process::MessageEvent& event);
  virtual void visit(const process::ExitedEvent& event);

  // Invoked when the message is ready to be executed after
  // being throttled.
  // 'principal' being None indicates it is throttled by
  // 'defaultLimiter'.
  void throttled(
      const process::MessageEvent& event,
      const Option<std::string>& principal);

  // Continuations of visit().
  void _visit(const process::MessageEvent& event);
  void _visit(const process::ExitedEvent& event);

  // Helper method invoked when the capacity for a framework
  // principal is exceeded.
  void exceededCapacity(
      const process::MessageEvent& event,
      const Option<std::string>& principal,
      uint64_t capacity);

  // Recovers state from the registrar.
  process::Future<Nothing> recover();
  void recoveredSlavesTimeout(const Registry& registry);

  void _registerSlave(
      const SlaveInfo& slaveInfo,
      const process::UPID& pid,
      const std::string& version,
      const process::Future<bool>& admit);

  void __reregisterSlave(
      Slave* slave,
      const std::vector<Task>& tasks);

  // 'promise' is used to signal finish of authentication.
  // 'future' is the future returned by the authenticator.
  void _authenticate(
      const process::UPID& pid,
      const process::Owned<process::Promise<Nothing>>& promise,
      const process::Future<Option<std::string>>& future);

  void authenticationTimeout(process::Future<Option<std::string>> future);

  void fileAttached(const process::Future<Nothing>& result,
                    const std::string& path);

  // Invoked when the contender has entered the contest.
  void contended(const process::Future<process::Future<Nothing>>& candidacy);

  // Task reconciliation, split from the message handler
  // to allow re-use.
  void _reconcileTasks(
      Framework* framework,
      const std::vector<TaskStatus>& statuses);

  // Handles a known re-registering slave by reconciling the master's
  // view of the slave's tasks and executors.
  void reconcile(
      Slave* slave,
      const std::vector<ExecutorInfo>& executors,
      const std::vector<Task>& tasks);

  // 'registerFramework()' continuation.
  void _registerFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      const process::Future<Option<Error>>& validationError);

  // 'reregisterFramework()' continuation.
  void _reregisterFramework(
      const process::UPID& from,
      const FrameworkInfo& frameworkInfo,
      bool failover,
      const process::Future<Option<Error>>& validationError);

  // Add a framework.
  void addFramework(Framework* framework);

  // Replace the scheduler for a framework with a new process ID, in
  // the event of a scheduler failover.
  void failoverFramework(Framework* framework, const process::UPID& newPid);

  // Kill all of a framework's tasks, delete the framework object, and
  // reschedule offers that were assigned to this framework.
  void removeFramework(Framework* framework);

  // Remove a framework from the slave, i.e., remove its tasks and
  // executors and recover the resources.
  void removeFramework(Slave* slave, Framework* framework);

  void disconnect(Framework* framework);
  void deactivate(Framework* framework);

  void disconnect(Slave* slave);
  void deactivate(Slave* slave);

  // Add a slave.
  void addSlave(
      Slave* slave,
      const std::vector<Archive::Framework>& completedFrameworks =
        std::vector<Archive::Framework>());

  // Remove the slave from the registrar and from the master's state.
  void removeSlave(Slave* slave);
  void _removeSlave(
      const SlaveInfo& slaveInfo,
      const std::vector<StatusUpdate>& updates,
      const process::Future<bool>& removed);

  // Validates the task.
  // Returns None if the task is valid.
  // Returns Error if the task is invalid.
  Option<Error> validateTask(
      const TaskInfo& task,
      Framework* framework,
      Slave* slave,
      const Resources& totalResources,
      const Resources& usedResources);

  // Authorizes the task.
  // Returns true if task is authorized.
  // Returns false if task is not authorized.
  // Returns failure for transient authorization failures.
  process::Future<bool> authorizeTask(
      const TaskInfo& task,
      Framework* framework);

  // 'launchTasks()' continuation.
  void _launchTasks(
      const FrameworkID& frameworkId,
      const SlaveID& slaveId,
      const std::vector<TaskInfo>& tasks,
      const Resources& totalResources,
      const Filters& filters,
      const process::Future<std::list<process::Future<bool>>>& authorizations);

  // Add the task and its executor (if not already running) to the
  // framework and slave. Returns the resources consumed as a result,
  // which includes resources for the task and its executor
  // (if not already running).
  Resources addTask(const TaskInfo& task, Framework* framework, Slave* slave);

  // Transitions the task, and recovers resources if the task becomes
  // terminal.
  void updateTask(Task* task, const StatusUpdate& update);

  // Removes the task.
  void removeTask(Task* task);

  // Remove an executor and recover its resources.
  void removeExecutor(
      Slave* slave,
      const FrameworkID& frameworkId,
      const ExecutorID& executorId);

  // Forwards the update to the framework.
  void forward(
      const StatusUpdate& update,
      const process::UPID& acknowledgee,
      Framework* framework);

  // Remove an offer after specified timeout
  void offerTimeout(const OfferID& offerId);

  // Remove an offer and optionally rescind the offer as well.
  void removeOffer(Offer* offer, bool rescind = false);

  Framework* getFramework(const FrameworkID& frameworkId);
  Slave* getSlave(const SlaveID& slaveId);
  Offer* getOffer(const OfferID& offerId);

  FrameworkID newFrameworkId();
  OfferID newOfferId();
  SlaveID newSlaveId();

  Option<Credentials> credentials;

private:
  // Inner class used to namespace HTTP route handlers (see
  // master/http.cpp for implementations).
  class Http
  {
  public:
    explicit Http(Master* _master) : master(_master) {}

    // /master/health
    process::Future<process::http::Response> health(
        const process::http::Request& request);

    // /master/observe
    process::Future<process::http::Response> observe(
        const process::http::Request& request);

    // /master/redirect
    process::Future<process::http::Response> redirect(
        const process::http::Request& request);

    // /master/roles.json
    process::Future<process::http::Response> roles(
        const process::http::Request& request);

    // /master/shutdown
    process::Future<process::http::Response> shutdown(
        const process::http::Request& request);

    // /master/state.json
    process::Future<process::http::Response> state(
        const process::http::Request& request);

    // /master/stats.json
    process::Future<process::http::Response> stats(
        const process::http::Request& request);

    // /master/tasks.json
    process::Future<process::http::Response> tasks(
        const process::http::Request& request);

    const static std::string HEALTH_HELP;
    const static std::string OBSERVE_HELP;
    const static std::string REDIRECT_HELP;
    const static std::string SHUTDOWN_HELP;
    const static std::string TASKS_HELP;

  private:
    // Helper for doing authentication, returns the credential used if
    // the authentication was successful (or none if no credentials
    // have been given to the master), otherwise an Error.
    Result<Credential> authenticate(const process::http::Request& request);

    // Continuations.
    process::Future<process::http::Response> _shutdown(
        const FrameworkID& id,
        bool authorized = true);

    Master* master;
  } http;

  Master(const Master&);              // No copying.
  Master& operator = (const Master&); // No assigning.

  friend struct OfferValidator;
  friend struct Metrics;

  const Flags flags;

  Option<MasterInfo> leader; // Current leading master.

  // Whether we are the current leading master.
  bool elected() const
  {
    return leader.isSome() && leader.get() == info_;
  }

  allocator::Allocator* allocator;
  WhitelistWatcher* whitelistWatcher;
  Registrar* registrar;
  Repairer* repairer;
  Files* files;

  MasterContender* contender;
  MasterDetector* detector;

  const Option<Authorizer*> authorizer;

  MasterInfo info_;

  // Indicates when recovery is complete. Recovery begins once the
  // master is elected as a leader.
  Option<process::Future<Nothing>> recovered;

  struct Slaves
  {
    Slaves() : removed(MAX_REMOVED_SLAVES) {}

    // Imposes a time limit for slaves that we recover from the
    // registry to re-register with the master.
    Option<process::Timer> recoveredTimer;

    // Slaves that have been recovered from the registrar but have yet
    // to re-register. We keep a "reregistrationTimer" above to ensure
    // we remove these slaves if they do not re-register.
    hashset<SlaveID> recovered;

    // Slaves that are in the process of registering.
    hashset<process::UPID> registering;

    // Only those slaves that are re-registering for the first time
    // with this master. We must not answer questions related to
    // these slaves until the registrar determines their fate.
    hashset<SlaveID> reregistering;

    hashmap<SlaveID, Slave*> registered;

    // Slaves that are in the process of being removed from the
    // registrar. Think of these as being partially removed: we must
    // not answer questions related to these until they are removed
    // from the registry.
    hashset<SlaveID> removing;

    // We track removed slaves to preserve the consistency
    // semantics of the pre-registrar code when a non-strict registrar
    // is being used. That is, if we remove a slave, we must make
    // an effort to prevent it from (re-)registering, sending updates,
    // etc. We keep a cache here to prevent this from growing in an
    // unbounded manner.
    // TODO(bmahler): Ideally we could use a cache with set semantics.
    Cache<SlaveID, Nothing> removed;

    bool transitioning(const Option<SlaveID>& slaveId)
    {
      if (slaveId.isSome()) {
        return recovered.contains(slaveId.get()) ||
               reregistering.contains(slaveId.get()) ||
               removing.contains(slaveId.get());
      } else {
        return !recovered.empty() ||
               !reregistering.empty() ||
               !removing.empty();
      }
    }
  } slaves;

  struct Frameworks
  {
    Frameworks() : completed(MAX_COMPLETED_FRAMEWORKS) {}

    hashmap<FrameworkID, Framework*> registered;
    boost::circular_buffer<memory::shared_ptr<Framework>> completed;

    // Principals of frameworks keyed by PID.
    // NOTE: Multiple PIDs can map to the same principal. The
    // principal is None when the framework doesn't specify it.
    // The differences between this map and 'authenticated' are:
    // 1) This map only includes *registered* frameworks. The mapping
    //    is added when a framework (re-)registers.
    // 2) This map includes unauthenticated frameworks (when Master
    //    allows them) if they have principals specified in
    //    FrameworkInfo.
    hashmap<process::UPID, Option<std::string>> principals;
  } frameworks;

  hashmap<OfferID, Offer*> offers;
  hashmap<OfferID, process::Timer> offerTimers;

  hashmap<std::string, Role*> roles;

  // Authenticator names as supplied via flags.
  std::vector<std::string> authenticatorNames;

  // Frameworks/slaves that are currently in the process of authentication.
  // 'authenticating' future for an authenticatee is ready when it is
  // authenticated.
  hashmap<process::UPID, process::Future<Nothing>> authenticating;

  hashmap<process::UPID, process::Owned<Authenticator>> authenticators;

  // Principals of authenticated frameworks/slaves keyed by PID.
  hashmap<process::UPID, std::string> authenticated;

  int64_t nextFrameworkId; // Used to give each framework a unique ID.
  int64_t nextOfferId;     // Used to give each slot offer a unique ID.
  int64_t nextSlaveId;     // Used to give each slave a unique ID.

  // TODO(bmahler): These are deprecated! Please use metrics instead.
  // Statistics (initialized in Master::initialize).
  struct
  {
    uint64_t tasks[TaskState_ARRAYSIZE];
    uint64_t validStatusUpdates;
    uint64_t invalidStatusUpdates;
    uint64_t validFrameworkMessages;
    uint64_t invalidFrameworkMessages;
  } stats;

  Metrics metrics;

  // Gauge handlers.
  double _uptime_secs()
  {
    return (process::Clock::now() - startTime).secs();
  }

  double _elected()
  {
    return elected() ? 1 : 0;
  }

  double _slaves_connected();
  double _slaves_disconnected();
  double _slaves_active();
  double _slaves_inactive();

  double _frameworks_connected();
  double _frameworks_disconnected();
  double _frameworks_active();
  double _frameworks_inactive();

  double _outstanding_offers()
  {
    return offers.size();
  }

  double _event_queue_messages()
  {
    return static_cast<double>(eventCount<process::MessageEvent>());
  }

  double _event_queue_dispatches()
  {
    return static_cast<double>(eventCount<process::DispatchEvent>());
  }

  double _event_queue_http_requests()
  {
    return static_cast<double>(eventCount<process::HttpEvent>());
  }

  double _tasks_staging();
  double _tasks_starting();
  double _tasks_running();

  double _resources_total(const std::string& name);
  double _resources_used(const std::string& name);
  double _resources_percent(const std::string& name);

  process::Time startTime; // Start time used to calculate uptime.

  Option<process::Time> electedTime; // Time when this master is elected.

  // Validates the framework including authorization.
  // Returns None if the framework is valid.
  // Returns Error if the framework is invalid.
  // Returns Failure if authorization returns 'Failure'.
  process::Future<Option<Error>> validate(
      const FrameworkInfo& frameworkInfo,
      const process::UPID& from);

  struct BoundedRateLimiter
  {
    BoundedRateLimiter(double qps, Option<uint64_t> _capacity)
      : limiter(new process::RateLimiter(qps)),
        capacity(_capacity),
        messages(0) {}

    process::Owned<process::RateLimiter> limiter;
    const Option<uint64_t> capacity;

    // Number of outstanding messages for this RateLimiter.
    // NOTE: ExitedEvents are throttled but not counted towards
    // the capacity here.
    uint64_t messages;
  };

  // BoundedRateLimiters keyed by the framework principal.
  // Like Metrics::Frameworks, all frameworks of the same principal
  // are throttled together at a common rate limit.
  hashmap<std::string, Option<process::Owned<BoundedRateLimiter>>> limiters;

  // The default limiter is for frameworks not specified in
  // 'flags.rate_limits'.
  Option<process::Owned<BoundedRateLimiter>> defaultLimiter;
};


struct Slave
{
  Slave(const SlaveInfo& _info,
        const process::UPID& _pid,
        const Option<std::string> _version,
        const process::Time& _registeredTime,
        const std::vector<ExecutorInfo> executorInfos =
          std::vector<ExecutorInfo>(),
        const std::vector<Task> tasks =
          std::vector<Task>())
    : id(_info.id()),
      info(_info),
      pid(_pid),
      version(_version),
      registeredTime(_registeredTime),
      connected(true),
      active(true),
      observer(NULL)
  {
    CHECK(_info.has_id());

    foreach (const ExecutorInfo& executorInfo, executorInfos) {
      CHECK(executorInfo.has_framework_id());
      addExecutor(executorInfo.framework_id(), executorInfo);
    }

    foreach (const Task& task, tasks) {
      addTask(new Task(task));
    }
  }

  ~Slave() {}

  Task* getTask(const FrameworkID& frameworkId, const TaskID& taskId)
  {
    if (tasks.contains(frameworkId) && tasks[frameworkId].contains(taskId)) {
      return tasks[frameworkId][taskId];
    }
    return NULL;
  }

  void addTask(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(!tasks[frameworkId].contains(taskId))
      << "Duplicate task " << taskId << " of framework " << frameworkId;

    tasks[frameworkId][taskId] = task;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources[frameworkId] += task->resources();
    }

    LOG(INFO) << "Adding task " << taskId
              << " with resources " << task->resources()
              << " on slave " << id << " (" << info.hostname() << ")";
  }

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    usedResources[frameworkId] -= task->resources();
    if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
      usedResources.erase(frameworkId);
    }
  }

  void removeTask(Task* task)
  {
    const TaskID& taskId = task->task_id();
    const FrameworkID& frameworkId = task->framework_id();

    CHECK(tasks[frameworkId].contains(taskId))
      << "Unknown task " << taskId << " of framework " << frameworkId;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources[frameworkId] -= task->resources();
      if (!tasks.contains(frameworkId) && !executors.contains(frameworkId)) {
        usedResources.erase(frameworkId);
      }
    }

    tasks[frameworkId].erase(taskId);
    if (tasks[frameworkId].empty()) {
      tasks.erase(frameworkId);
    }

    killedTasks.remove(frameworkId, taskId);
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();

    offers.insert(offer);
    offeredResources += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.contains(offer)) << "Unknown offer " << offer->id();

    offeredResources -= offer->resources();
    offers.erase(offer);
  }

  bool hasExecutor(const FrameworkID& frameworkId,
                   const ExecutorID& executorId) const
  {
    return executors.contains(frameworkId) &&
      executors.get(frameworkId).get().contains(executorId);
  }

  void addExecutor(const FrameworkID& frameworkId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(frameworkId, executorInfo.executor_id()))
      << "Duplicate executor " << executorInfo.executor_id()
      << " of framework " << frameworkId;

    executors[frameworkId][executorInfo.executor_id()] = executorInfo;
    usedResources[frameworkId] += executorInfo.resources();
  }

  void removeExecutor(const FrameworkID& frameworkId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(frameworkId, executorId))
      << "Unknown executor " << executorId << " of framework " << frameworkId;

    usedResources[frameworkId] -=
      executors[frameworkId][executorId].resources();

    // XXX Remove.

    executors[frameworkId].erase(executorId);
    if (executors[frameworkId].empty()) {
      executors.erase(frameworkId);
    }
  }

  const SlaveID id;
  const SlaveInfo info;

  process::UPID pid;

  // The Mesos version of the slave. If set, the slave is >= 0.21.0.
  // TODO(bmahler): Use stout's Version when it can parse labels, etc.
  // TODO(bmahler): Make this required once it is always set.
  const Option<std::string> version;

  process::Time registeredTime;
  Option<process::Time> reregisteredTime;

  // Slave becomes disconnected when the socket closes.
  bool connected;

  // Slave becomes deactivated when it gets disconnected. In the
  // future this might also happen via HTTP endpoint.
  // No offers will be made for a deactivated slave.
  bool active;

  // Executors running on this slave.
  hashmap<FrameworkID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // Tasks present on this slave.
  // TODO(bmahler): The task pointer ownership complexity arises from the fact
  // that we own the pointer here, but it's shared with the Framework struct.
  // We should find a way to eliminate this.
  hashmap<FrameworkID, hashmap<TaskID, Task*>> tasks;

  // Tasks that were asked to kill by frameworks.
  // This is used for reconciliation when the slave re-registers.
  multihashmap<FrameworkID, TaskID> killedTasks;

  // Active offers on this slave.
  hashset<Offer*> offers;

  hashmap<FrameworkID, Resources> usedResources;  // Active task / executors.
  Resources offeredResources; // Offers.

  SlaveObserver* observer;

private:
  Slave(const Slave&);              // No copying.
  Slave& operator = (const Slave&); // No assigning.
};


inline std::ostream& operator << (std::ostream& stream, const Slave& slave)
{
  return stream << slave.id << " at " << slave.pid
                << " (" << slave.info.hostname() << ")";
}


// Information about a connected or completed framework.
// TODO(bmahler): Keeping the task and executor information in sync
// across the Slave and Framework structs is error prone!
struct Framework
{
  Framework(const FrameworkInfo& _info,
            const FrameworkID& _id,
            const process::UPID& _pid,
            const process::Time& time = process::Clock::now())
    : id(_id),
      info(_info),
      pid(_pid),
      connected(true),
      active(true),
      registeredTime(time),
      reregisteredTime(time),
      completedTasks(MAX_COMPLETED_TASKS_PER_FRAMEWORK) {}

  ~Framework() {}

  Task* getTask(const TaskID& taskId)
  {
    if (tasks.count(taskId) > 0) {
      return tasks[taskId];
    } else {
      return NULL;
    }
  }

  void addTask(Task* task)
  {
    CHECK(!tasks.contains(task->task_id()))
      << "Duplicate task " << task->task_id()
      << " of framework " << task->framework_id();

    tasks[task->task_id()] = task;

    if (!protobuf::isTerminalState(task->state())) {
      usedResources += task->resources();
    }
  }

  // Notification of task termination, for resource accounting.
  // TODO(bmahler): This is a hack for performance. We need to
  // maintain resource counters because computing task resources
  // functionally for all tasks is expensive, for now.
  void taskTerminated(Task* task)
  {
    CHECK(protobuf::isTerminalState(task->state()));
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    usedResources -= task->resources();
  }

  void addCompletedTask(const Task& task)
  {
    // TODO(adam-mesos): Check if completed task already exists.
    completedTasks.push_back(memory::shared_ptr<Task>(new Task(task)));
  }

  void removeTask(Task* task)
  {
    CHECK(tasks.contains(task->task_id()))
      << "Unknown task " << task->task_id()
      << " of framework " << task->framework_id();

    if (!protobuf::isTerminalState(task->state())) {
      usedResources -= task->resources();
    }

    addCompletedTask(*task);

    tasks.erase(task->task_id());
  }

  void addOffer(Offer* offer)
  {
    CHECK(!offers.contains(offer)) << "Duplicate offer " << offer->id();
    offers.insert(offer);
    offeredResources += offer->resources();
  }

  void removeOffer(Offer* offer)
  {
    CHECK(offers.find(offer) != offers.end())
      << "Unknown offer " << offer->id();

    offeredResources -= offer->resources();
    offers.erase(offer);
  }

  bool hasExecutor(const SlaveID& slaveId,
                   const ExecutorID& executorId)
  {
    return executors.contains(slaveId) &&
      executors[slaveId].contains(executorId);
  }

  void addExecutor(const SlaveID& slaveId,
                   const ExecutorInfo& executorInfo)
  {
    CHECK(!hasExecutor(slaveId, executorInfo.executor_id()))
      << "Duplicate executor " << executorInfo.executor_id()
      << " on slave " << slaveId;

    executors[slaveId][executorInfo.executor_id()] = executorInfo;
    usedResources += executorInfo.resources();
  }

  void removeExecutor(const SlaveID& slaveId,
                      const ExecutorID& executorId)
  {
    CHECK(hasExecutor(slaveId, executorId))
      << "Unknown executor " << executorId
      << " of framework " << id
      << " of slave " << slaveId;

    usedResources -= executors[slaveId][executorId].resources();
    executors[slaveId].erase(executorId);
    if (executors[slaveId].empty()) {
      executors.erase(slaveId);
    }
  }

  const FrameworkID id; // TODO(benh): Store this in 'info'.

  const FrameworkInfo info;

  process::UPID pid;

  // Framework becomes disconnected when the socket closes.
  bool connected;

  // Framework becomes deactivated when it is disconnected or
  // the master receives a DeactivateFrameworkMessage.
  // No offers will be made to a deactivated framework.
  bool active;

  process::Time registeredTime;
  process::Time reregisteredTime;
  process::Time unregisteredTime;

  // Tasks that have not yet been launched because they are currently
  // being authorized.
  hashmap<TaskID, TaskInfo> pendingTasks;

  hashmap<TaskID, Task*> tasks;

  // NOTE: We use a shared pointer for Task because clang doesn't like
  // Boost's implementation of circular_buffer with Task (Boost
  // attempts to do some memset's which are unsafe).
  boost::circular_buffer<memory::shared_ptr<Task>> completedTasks;

  hashset<Offer*> offers; // Active offers for framework.

  hashmap<SlaveID, hashmap<ExecutorID, ExecutorInfo>> executors;

  // TODO(bmahler): Summing set and ranges resources across slaves
  // does not yield meaningful totals.
  Resources usedResources;    // Active task / executor resources.
  Resources offeredResources; // Offered resources.

private:
  Framework(const Framework&);              // No copying.
  Framework& operator = (const Framework&); // No assigning.
};


inline std::ostream& operator << (
    std::ostream& stream,
    const Framework& framework)
{
  // TODO(vinod): Also log the hostname once FrameworkInfo is properly
  // updated on framework failover (MESOS-1784).
  return stream << framework.id << " (" << framework.info.name()
                << ") at " << framework.pid;
}


// Information about an active role.
struct Role
{
  explicit Role(const RoleInfo& _info)
    : info(_info) {}

  void addFramework(Framework* framework)
  {
    frameworks[framework->id] = framework;
  }

  void removeFramework(Framework* framework)
  {
    frameworks.erase(framework->id);
  }

  Resources resources() const
  {
    Resources resources;
    foreachvalue (Framework* framework, frameworks) {
      resources += framework->usedResources;
      resources += framework->offeredResources;
    }

    return resources;
  }

  RoleInfo info;

  hashmap<FrameworkID, Framework*> frameworks;
};


// Implementation of slave admission Registrar operation.
class AdmitSlave : public Operation
{
public:
  explicit AdmitSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    // Check and see if this slave already exists.
    if (slaveIDs->contains(info.id())) {
      if (strict) {
        return Error("Slave already admitted");
      } else {
        return false; // No mutation.
      }
    }

    Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
    slave->mutable_info()->CopyFrom(info);
    slaveIDs->insert(info.id());
    return true; // Mutation.
  }

private:
  const SlaveInfo info;
};


// Implementation of slave readmission Registrar operation.
class ReadmitSlave : public Operation
{
public:
  explicit ReadmitSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    if (slaveIDs->contains(info.id())) {
      return false; // No mutation.
    }

    if (strict) {
      return Error("Slave not yet admitted");
    } else {
      Registry::Slave* slave = registry->mutable_slaves()->add_slaves();
      slave->mutable_info()->CopyFrom(info);
      slaveIDs->insert(info.id());
      return true; // Mutation.
    }
  }

private:
  const SlaveInfo info;
};


// Implementation of slave removal Registrar operation.
class RemoveSlave : public Operation
{
public:
  explicit RemoveSlave(const SlaveInfo& _info) : info(_info)
  {
    CHECK(info.has_id()) << "SlaveInfo is missing the 'id' field";
  }

protected:
  virtual Try<bool> perform(
      Registry* registry,
      hashset<SlaveID>* slaveIDs,
      bool strict)
  {
    for (int i = 0; i < registry->slaves().slaves().size(); i++) {
      const Registry::Slave& slave = registry->slaves().slaves(i);
      if (slave.info().id() == info.id()) {
        registry->mutable_slaves()->mutable_slaves()->DeleteSubrange(i, 1);
        slaveIDs->erase(info.id());
        return true; // Mutation.
      }
    }

    if (strict) {
      return Error("Slave not yet admitted");
    } else {
      return false; // No mutation.
    }
  }

private:
  const SlaveInfo info;
};

} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HPP__
