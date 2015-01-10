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

#include <stdint.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <list>
#include <sstream>

#include <mesos/module.hpp>

#include <process/check.hpp>
#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/limiter.hpp>
#include <process/owned.hpp>
#include <process/run.hpp>
#include <process/shared.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/memory.hpp>
#include <stout/multihashmap.hpp>
#include <stout/net.hpp>
#include <stout/nothing.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "authentication/authenticator.hpp"
#include "authentication/cram_md5/authenticator.hpp"

#include "authorizer/authorizer.hpp"

#include "common/build.hpp"
#include "common/date_utils.hpp"
#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "credentials/credentials.hpp"

#include "logging/flags.hpp"
#include "logging/logging.hpp"

#include "master/allocator.hpp"
#include "master/flags.hpp"
#include "master/master.hpp"

#include "module/authenticator.hpp"
#include "module/manager.hpp"

#include "watcher/whitelist_watcher.hpp"

using std::list;
using std::string;
using std::vector;

using process::await;
using process::wait; // Necessary on some OS's to disambiguate.
using process::Clock;
using process::ExitedEvent;
using process::Failure;
using process::Future;
using process::MessageEvent;
using process::Owned;
using process::PID;
using process::Process;
using process::Promise;
using process::Shared;
using process::Time;
using process::Timer;
using process::UPID;

using process::metrics::Counter;

using memory::shared_ptr;

namespace mesos {
namespace internal {
namespace master {

using allocator::Allocator;


class SlaveObserver : public Process<SlaveObserver>
{
public:
  SlaveObserver(const UPID& _slave,
                const SlaveInfo& _slaveInfo,
                const SlaveID& _slaveId,
                const PID<Master>& _master)
    : ProcessBase(process::ID::generate("slave-observer")),
      slave(_slave),
      slaveInfo(_slaveInfo),
      slaveId(_slaveId),
      master(_master),
      timeouts(0),
      pinged(false),
      connected(true)
  {
    // TODO(vinod): Deprecate this handler in 0.22.0 in favor of a
    // new PongSlaveMessage handler.
    install("PONG", &SlaveObserver::pong);
  }

  void reconnect()
  {
    connected = true;
  }

  void disconnect()
  {
    connected = false;
  }

protected:
  virtual void initialize()
  {
    ping();
  }

  void ping()
  {
    // TODO(vinod): In 0.22.0, master should send the PingSlaveMessage
    // instead of sending "PING" with the encoded PingSlaveMessage.
    // Currently we do not do this for backwards compatibility with
    // slaves on 0.20.0.
    PingSlaveMessage message;
    message.set_connected(connected);
    string data;
    CHECK(message.SerializeToString(&data));
    send(slave, "PING", data.data(), data.size());

    pinged = true;
    delay(SLAVE_PING_TIMEOUT, self(), &SlaveObserver::timeout);
  }

  void pong(const UPID& from, const string& body)
  {
    timeouts = 0;
    pinged = false;
  }

  void timeout()
  {
    if (pinged) { // So we haven't got back a pong yet ...
      if (++timeouts >= MAX_SLAVE_PING_TIMEOUTS) {
        shutdown();
        return;
      }
    }

    ping();
  }

  void shutdown()
  {
    dispatch(master, &Master::shutdownSlave, slaveId, "health check timed out");
  }

private:
  const UPID slave;
  const SlaveInfo slaveInfo;
  const SlaveID slaveId;
  const PID<Master> master;
  uint32_t timeouts;
  bool pinged;
  bool connected;
};


Master::Master(
    Allocator* _allocator,
    Registrar* _registrar,
    Repairer* _repairer,
    Files* _files,
    MasterContender* _contender,
    MasterDetector* _detector,
    const Option<Authorizer*>& _authorizer,
    const Flags& _flags)
  : ProcessBase("master"),
    http(this),
    flags(_flags),
    allocator(_allocator),
    registrar(_registrar),
    repairer(_repairer),
    files(_files),
    contender(_contender),
    detector(_detector),
    authorizer(_authorizer),
    metrics(*this),
    electedTime(None())
{
  // NOTE: We populate 'info_' here instead of inside 'initialize()'
  // because 'StandaloneMasterDetector' needs access to the info.

  // The master ID is currently comprised of the current date, the IP
  // address and port from self() and the OS PID.
  Try<string> id =
    strings::format("%s-%u-%u-%d", DateUtils::currentDate(),
                    self().node.ip, self().node.port, getpid());

  CHECK(!id.isError()) << id.error();

  info_.set_id(id.get());
  info_.set_ip(self().node.ip);
  info_.set_port(self().node.port);
  info_.set_pid(self());

  // Determine our hostname or use the hostname provided.
  string hostname;

  if (flags.hostname.isNone()) {
    Try<string> result = net::getHostname(self().node.ip);

    if (result.isError()) {
      LOG(FATAL) << "Failed to get hostname: " << result.error();
    }

    hostname = result.get();
  } else {
    hostname = flags.hostname.get();
  }

  info_.set_hostname(hostname);
}


Master::~Master() {}


void Master::initialize()
{
  LOG(INFO) << "Master " << info_.id() << " (" << info_.hostname() << ")"
            << " started on " << string(self()).substr(7);

  if (stringify(net::IP(ntohl(self().node.ip))) == "127.0.0.1") {
    LOG(WARNING) << "\n**************************************************\n"
                 << "Master bound to loopback interface!"
                 << " Cannot communicate with remote schedulers or slaves."
                 << " You might want to set '--ip' flag to a routable"
                 << " IP address.\n"
                 << "**************************************************";
  }

  // NOTE: We enforce a minimum slave re-register timeout because the
  // slave bounds its (re-)registration retries based on the minimum.
  if (flags.slave_reregister_timeout < MIN_SLAVE_REREGISTER_TIMEOUT) {
    EXIT(1) << "Invalid value '" << flags.slave_reregister_timeout << "' "
            << "for --slave_reregister_timeout: "
            << "Must be at least " << MIN_SLAVE_REREGISTER_TIMEOUT;
  }

  // Parse the percentage for the slave removal limit.
  // TODO(bmahler): Add a 'Percentage' abstraction.
  if (!strings::endsWith(flags.recovery_slave_removal_limit, "%")) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: " << "missing '%'";
  }

  Try<double> limit = numify<double>(
      strings::remove(
          flags.recovery_slave_removal_limit,
          "%",
          strings::SUFFIX));

  if (limit.isError()) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: " << limit.error();
  }

  if (limit.get() < 0.0 || limit.get() > 100.0) {
    EXIT(1) << "Invalid value '" << flags.recovery_slave_removal_limit << "' "
            << "for --recovery_slave_removal_percent_limit: "
            << "Must be within [0%-100%]";
  }

  // Log authentication state.
  if (flags.authenticate_frameworks) {
    LOG(INFO) << "Master only allowing authenticated frameworks to register";
  } else {
    LOG(INFO) << "Master allowing unauthenticated frameworks to register";
  }
  if (flags.authenticate_slaves) {
    LOG(INFO) << "Master only allowing authenticated slaves to register";
  } else {
    LOG(INFO) << "Master allowing unauthenticated slaves to register";
  }

  // Extract authenticator names and validate them.
  authenticatorNames = strings::split(flags.authenticators, ",");
  if (authenticatorNames.empty()) {
    EXIT(1) << "No authenticator specified";
  }
  if (authenticatorNames.size() > 1) {
    EXIT(1) << "Multiple authenticators not supported";
  }
  if (authenticatorNames[0] != DEFAULT_AUTHENTICATOR &&
      !modules::ModuleManager::contains<Authenticator>(
          authenticatorNames[0])) {
    EXIT(1) << "Authenticator '" << authenticatorNames[0] << "' not found. "
            << "Check the spelling (compare to '" << DEFAULT_AUTHENTICATOR
            << "'') or verify that the authenticator was loaded successfully "
            << "(see --modules)";
  }

  // Load credentials.
  if (flags.credentials.isSome()) {
    const string& path =
      strings::remove(flags.credentials.get(), "file://", strings::PREFIX);

    Result<Credentials> _credentials = credentials::read(path);
    if (_credentials.isError()) {
      EXIT(1) << _credentials.error() << " (see --credentials flag)";
    } else if (_credentials.isNone()) {
      EXIT(1) << "Credentials file must contain at least one credential"
              << " (see --credentials flag)";
    }
    // Store credentials in master to use them in routes.
    credentials = _credentials.get();

    // Give Authenticator access to credentials.
    // TODO(tillt): Move this into a mechanism (module) specific
    // Authenticator factory. See MESOS-2050.
    cram_md5::secrets::load(credentials.get());
  }

  if (authorizer.isSome()) {
    LOG(INFO) << "Authorization enabled";
  }

  if (flags.rate_limits.isSome()) {
    // Add framework rate limiters.
    foreach (const RateLimit& limit_, flags.rate_limits.get().limits()) {
      if (limiters.contains(limit_.principal())) {
        EXIT(1) << "Duplicate principal " << limit_.principal()
                << " found in RateLimits configuration";
      }

      if (limit_.has_qps() && limit_.qps() <= 0) {
        EXIT(1) << "Invalid qps: " << limit_.qps()
                << ". It must be a positive number";
      }

      if (limit_.has_qps()) {
        Option<uint64_t> capacity;
        if (limit_.has_capacity()) {
          capacity = limit_.capacity();
        }
        limiters.put(
            limit_.principal(),
            Owned<BoundedRateLimiter>(
                new BoundedRateLimiter(limit_.qps(), capacity)));
      } else {
        limiters.put(limit_.principal(), None());
      }
    }

    if (flags.rate_limits.get().has_aggregate_default_qps() &&
        flags.rate_limits.get().aggregate_default_qps() <= 0) {
      EXIT(1) << "Invalid aggregate_default_qps: "
              << flags.rate_limits.get().aggregate_default_qps()
              << ". It must be a positive number";
    }

    if (flags.rate_limits.get().has_aggregate_default_qps()) {
      Option<uint64_t> capacity;
      if (flags.rate_limits.get().has_aggregate_default_capacity()) {
        capacity = flags.rate_limits.get().aggregate_default_capacity();
      }
      defaultLimiter = Owned<BoundedRateLimiter>(
          new BoundedRateLimiter(
              flags.rate_limits.get().aggregate_default_qps(), capacity));
    }

    LOG(INFO) << "Framework rate limiting enabled";
  }

  hashmap<string, RoleInfo> roleInfos;

  // Add the default role.
  RoleInfo roleInfo;
  roleInfo.set_name("*");
  roleInfos["*"] = roleInfo;

  // Add other roles.
  if (flags.roles.isSome()) {
    vector<string> tokens = strings::tokenize(flags.roles.get(), ",");

    foreach (const std::string& role, tokens) {
      RoleInfo roleInfo;
      roleInfo.set_name(role);
      roleInfos[role] = roleInfo;
    }
  }

  // Add role weights.
  if (flags.weights.isSome()) {
    vector<string> tokens = strings::tokenize(flags.weights.get(), ",");

    foreach (const std::string& token, tokens) {
      vector<string> pair = strings::tokenize(token, "=");
      if (pair.size() != 2) {
        EXIT(1) << "Invalid weight: '" << token << "'. --weights should"
          "be of the form 'role=weight,role=weight'\n";
      } else if (!roleInfos.contains(pair[0])) {
        EXIT(1) << "Invalid weight: '" << token << "'. " << pair[0]
                << " is not a valid role.";
      }

      double weight = atof(pair[1].c_str());
      if (weight <= 0) {
        EXIT(1) << "Invalid weight: '" << token
                << "'. Weights must be positive.";
      }

      roleInfos[pair[0]].set_weight(weight);
    }
  }

  foreachpair (const std::string& role,
               const RoleInfo& roleInfo,
               roleInfos) {
    roles[role] = new Role(roleInfo);
  }

  // Verify the timeout is greater than zero.
  if (flags.offer_timeout.isSome() &&
      flags.offer_timeout.get() <= Duration::zero()) {
    EXIT(1) << "Invalid value '" << flags.offer_timeout.get() << "' "
            << "for --offer_timeout: Must be greater than zero.";
  }

  // Initialize the allocator.
  allocator->initialize(
      flags,
      defer(self(), &Master::offer, lambda::_1, lambda::_2),
      roleInfos);

  // Parse the whitelist. Passing allocator::updateWhitelist()
  // callback is safe because we shut down the whitelistWatcher in
  // Master::finalize(), while allocator lifetime is greater than
  // masters. Therefore there is no risk of calling into an allocator
  // that has been cleaned up.
  whitelistWatcher = new WhitelistWatcher(
      flags.whitelist,
      WHITELIST_WATCH_INTERVAL,
      lambda::bind(&Allocator::updateWhitelist, allocator, lambda::_1));
  spawn(whitelistWatcher);

  nextFrameworkId = 0;
  nextSlaveId = 0;
  nextOfferId = 0;

  // Start all the statistics at 0.
  stats.tasks[TASK_STAGING] = 0;
  stats.tasks[TASK_STARTING] = 0;
  stats.tasks[TASK_RUNNING] = 0;
  stats.tasks[TASK_FINISHED] = 0;
  stats.tasks[TASK_FAILED] = 0;
  stats.tasks[TASK_KILLED] = 0;
  stats.tasks[TASK_LOST] = 0;
  stats.validStatusUpdates = 0;
  stats.invalidStatusUpdates = 0;
  stats.validFrameworkMessages = 0;
  stats.invalidFrameworkMessages = 0;

  startTime = Clock::now();

  // Install handler functions for certain messages.
  install<SubmitSchedulerRequest>(
      &Master::submitScheduler,
      &SubmitSchedulerRequest::name);

  install<RegisterFrameworkMessage>(
      &Master::registerFramework,
      &RegisterFrameworkMessage::framework);

  install<ReregisterFrameworkMessage>(
      &Master::reregisterFramework,
      &ReregisterFrameworkMessage::framework,
      &ReregisterFrameworkMessage::failover);

  install<UnregisterFrameworkMessage>(
      &Master::unregisterFramework,
      &UnregisterFrameworkMessage::framework_id);

  install<DeactivateFrameworkMessage>(
        &Master::deactivateFramework,
        &DeactivateFrameworkMessage::framework_id);

  install<ResourceRequestMessage>(
      &Master::resourceRequest,
      &ResourceRequestMessage::framework_id,
      &ResourceRequestMessage::requests);

  install<LaunchTasksMessage>(
      &Master::launchTasks,
      &LaunchTasksMessage::framework_id,
      &LaunchTasksMessage::tasks,
      &LaunchTasksMessage::filters,
      &LaunchTasksMessage::offer_ids);

  install<ReviveOffersMessage>(
      &Master::reviveOffers,
      &ReviveOffersMessage::framework_id);

  install<KillTaskMessage>(
      &Master::killTask,
      &KillTaskMessage::framework_id,
      &KillTaskMessage::task_id);

  install<StatusUpdateAcknowledgementMessage>(
      &Master::statusUpdateAcknowledgement,
      &StatusUpdateAcknowledgementMessage::slave_id,
      &StatusUpdateAcknowledgementMessage::framework_id,
      &StatusUpdateAcknowledgementMessage::task_id,
      &StatusUpdateAcknowledgementMessage::uuid);

  install<FrameworkToExecutorMessage>(
      &Master::schedulerMessage,
      &FrameworkToExecutorMessage::slave_id,
      &FrameworkToExecutorMessage::framework_id,
      &FrameworkToExecutorMessage::executor_id,
      &FrameworkToExecutorMessage::data);

  install<RegisterSlaveMessage>(
      &Master::registerSlave,
      &RegisterSlaveMessage::slave,
      &RegisterSlaveMessage::version);

  install<ReregisterSlaveMessage>(
      &Master::reregisterSlave,
      &ReregisterSlaveMessage::slave,
      &ReregisterSlaveMessage::executor_infos,
      &ReregisterSlaveMessage::tasks,
      &ReregisterSlaveMessage::completed_frameworks,
      &ReregisterSlaveMessage::version);

  install<UnregisterSlaveMessage>(
      &Master::unregisterSlave,
      &UnregisterSlaveMessage::slave_id);

  install<StatusUpdateMessage>(
      &Master::statusUpdate,
      &StatusUpdateMessage::update,
      &StatusUpdateMessage::pid);

  install<ReconcileTasksMessage>(
      &Master::reconcileTasks,
      &ReconcileTasksMessage::framework_id,
      &ReconcileTasksMessage::statuses);

  install<ExitedExecutorMessage>(
      &Master::exitedExecutor,
      &ExitedExecutorMessage::slave_id,
      &ExitedExecutorMessage::framework_id,
      &ExitedExecutorMessage::executor_id,
      &ExitedExecutorMessage::status);

  install<AuthenticateMessage>(
      &Master::authenticate,
      &AuthenticateMessage::pid);

  // Setup HTTP routes.
  route("/health",
        Http::HEALTH_HELP,
        lambda::bind(&Http::health, http, lambda::_1));
  route("/observe",
        Http::OBSERVE_HELP,
        lambda::bind(&Http::observe, http, lambda::_1));
  route("/redirect",
        Http::REDIRECT_HELP,
        lambda::bind(&Http::redirect, http, lambda::_1));
  route("/roles.json",
        None(),
        lambda::bind(&Http::roles, http, lambda::_1));
  route("/shutdown",
        Http::SHUTDOWN_HELP,
        lambda::bind(&Http::shutdown, http, lambda::_1));
  route("/state.json",
        None(),
        lambda::bind(&Http::state, http, lambda::_1));
  route("/stats.json",
        None(),
        lambda::bind(&Http::stats, http, lambda::_1));
  route("/tasks.json",
        Http::TASKS_HELP,
        lambda::bind(&Http::tasks, http, lambda::_1));

  // Provide HTTP assets from a "webui" directory. This is either
  // specified via flags (which is necessary for running out of the
  // build directory before 'make install') or determined at build
  // time via the preprocessor macro '-DMESOS_WEBUI_DIR' set in the
  // Makefile.
  provide("", path::join(flags.webui_dir, "master/static/index.html"));
  provide("static", path::join(flags.webui_dir, "master/static"));

  if (flags.log_dir.isSome()) {
    Try<string> log = logging::getLogFile(
        logging::getLogSeverity(flags.logging_level));

    if (log.isError()) {
      LOG(ERROR) << "Master log file cannot be found: " << log.error();
    } else {
      files->attach(log.get(), "/master/log")
        .onAny(defer(self(), &Self::fileAttached, lambda::_1, log.get()));
    }
  }

  contender->initialize(info_);

  // Start contending to be a leading master and detecting the current
  // leader.
  contender->contend()
    .onAny(defer(self(), &Master::contended, lambda::_1));
  detector->detect()
    .onAny(defer(self(), &Master::detected, lambda::_1));
}


void Master::finalize()
{
  LOG(INFO) << "Master terminating";

  // Remove the slaves.
  foreachvalue (Slave* slave, slaves.registered) {
    // Remove tasks, don't bother recovering resources.
    foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
      foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
        removeTask(task);
      }
    }

    // Remove executors.
    foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
      foreachkey (const ExecutorID& executorId,
                  utils::copy(slave->executors[frameworkId])) {
        removeExecutor(slave, frameworkId, executorId);
      }
    }

    // Remove offers.
    foreach (Offer* offer, utils::copy(slave->offers)) {
      removeOffer(offer);
    }

    // Terminate the slave observer.
    terminate(slave->observer);
    wait(slave->observer);

    delete slave->observer;
    delete slave;
  }
  slaves.registered.clear();

  // Remove the frameworks.
  // Note we are not deleting the pointers to the frameworks from the
  // allocator or the roles because it is unnecessary bookkeeping at
  // this point since we are shutting down.
  foreachvalue (Framework* framework, frameworks.registered) {
    // Remove pending tasks from the framework. Don't bother
    // recovering the resources in the allocator.
    framework->pendingTasks.clear();

    // No tasks/executors/offers should remain since the slaves
    // have been removed.
    CHECK(framework->tasks.empty());
    CHECK(framework->executors.empty());
    CHECK(framework->offers.empty());

    delete framework;
  }
  frameworks.registered.clear();

  CHECK(offers.empty());

  foreachvalue (Future<Nothing> future, authenticating) {
    // NOTE: This is necessary during tests because a copy of
    // this future is used to setup authentication timeout. If a
    // test doesn't discard this future, authentication timeout might
    // fire in a different test and any associated callbacks
    // (e.g., '_authenticate()') would be called. This is because the
    // master pid doesn't change across the tests.
    // TODO(vinod): This seems to be a bug in libprocess or the
    // testing infrastructure.
    future.discard();
  }

  foreachvalue (Role* role, roles) {
    delete role;
  }
  roles.clear();

  // NOTE: This is necessary during tests because we don't want the
  // timer to fire in a different test and invoke the callback.
  // The callback would be invoked because the master pid doesn't
  // change across the tests.
  // TODO(vinod): This seems to be a bug in libprocess or the
  // testing infrastructure.
  if (slaves.recoveredTimer.isSome()) {
    Clock::cancel(slaves.recoveredTimer.get());
  }

  terminate(whitelistWatcher);
  wait(whitelistWatcher);
  delete whitelistWatcher;
}


void Master::exited(const UPID& pid)
{
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->pid == pid) {
      LOG(INFO) << "Framework " << *framework << " disconnected";

      // Disconnect the framework.
      disconnect(framework);

      // Set 'failoverTimeout' to the default and update only if the
      // input is valid.
      Try<Duration> failoverTimeout_ =
        Duration::create(FrameworkInfo().failover_timeout());
      CHECK_SOME(failoverTimeout_);
      Duration failoverTimeout = failoverTimeout_.get();

      failoverTimeout_ =
        Duration::create(framework->info.failover_timeout());
      if (failoverTimeout_.isSome()) {
        failoverTimeout = failoverTimeout_.get();
      } else {
        LOG(WARNING) << "Using the default value for 'failover_timeout' because"
                     << "the input value is invalid: "
                     << failoverTimeout_.error();
      }

      LOG(INFO) << "Giving framework " << *framework << " "
                << failoverTimeout << " to failover";

      // Delay dispatching a message to ourselves for the timeout.
      delay(failoverTimeout,
          self(),
          &Master::frameworkFailoverTimeout,
          framework->id,
          framework->reregisteredTime);

      return;
    }
  }

  // The semantics when a slave gets disconnected are as follows:
  // 1) If the slave is not checkpointing, the slave is immediately
  //    removed and all tasks running on it are transitioned to LOST.
  //    No resources are recovered, because the slave is removed.
  // 2) If the slave is checkpointing, the frameworks running on it
  //    fall into one of the 2 cases:
  //    2.1) Framework is checkpointing: No immediate action is taken.
  //         The slave is given a chance to reconnect until the slave
  //         observer times out (75s) and removes the slave (Case 1).
  //    2.2) Framework is not-checkpointing: The slave is not removed
  //         but the framework is removed from the slave's structs,
  //         its tasks transitioned to LOST and resources recovered.
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->pid == pid) {
      LOG(INFO) << "Slave " << *slave << " disconnected";

      if (!slave->info.checkpoint()) {
        // Remove the slave, if it is not checkpointing.
        LOG(INFO) << "Removing disconnected slave " << *slave
                  << " because it is not checkpointing!";
        removeSlave(slave);
        return;
      } else if (slave->connected) {
        // Checkpointing slaves can just be disconnected.
        disconnect(slave);

        // Remove all non-checkpointing frameworks.
        hashset<FrameworkID> frameworkIds =
          slave->tasks.keys() | slave->executors.keys();

        foreach (const FrameworkID& frameworkId, frameworkIds) {
          Framework* framework = getFramework(frameworkId);
          if (framework != NULL && !framework->info.checkpoint()) {
            LOG(INFO) << "Removing framework " << *framework
                      << " from disconnected slave " << *slave
                      << " because the framework is not checkpointing";

            removeFramework(slave, framework);
          }
        }
      } else {
        // NOTE: A duplicate exited() event is possible for a slave
        // because its PID doesn't change on restart. See MESOS-675
        // for details.
        LOG(WARNING) << "Ignoring duplicate exited() notification for "
                     << "checkpointing slave " << *slave;
      }
    }
  }
}


void Master::visit(const MessageEvent& event)
{
  // There are three cases about the message's UPID with respect to
  // 'frameworks.principals':
  // 1) if a <UPID, principal> pair exists and the principal is Some,
  //    it's a framework with its principal specified.
  // 2) if a <UPID, principal> pair exists and the principal is None,
  //    it's a framework without a principal.
  // 3) if a <UPID, principal> pair does not exist in the map, it's
  //    either an unregistered framework or not a framework.
  // The logic for framework message counters and rate limiting
  // mainly concerns with whether the UPID is a *registered*
  // framework and whether the framework has a principal so we use
  // these two temp variables to simplify the condition checks below.
  bool isRegisteredFramework =
    frameworks.principals.contains(event.message->from);
  const Option<string> principal = isRegisteredFramework
    ? frameworks.principals[event.message->from]
    : Option<string>::none();

  // Increment the "message_received" counter if the message is from
  // a framework and such a counter is configured for it.
  // See comments for 'Master::Metrics::Frameworks' and
  // 'Master::Frameworks::principals' for details.
  if (principal.isSome()) {
    // If the framework has a principal, the counter must exist.
    CHECK(metrics.frameworks.contains(principal.get()));
    Counter messages_received =
      metrics.frameworks.get(principal.get()).get()->messages_received;
    ++messages_received;
  }

  // All messages are filtered when non-leading.
  if (!elected()) {
    VLOG(1) << "Dropping '" << event.message->name << "' message since "
            << "not elected yet";
    ++metrics.dropped_messages;
    return;
  }

  CHECK_SOME(recovered);

  // All messages are filtered while recovering.
  // TODO(bmahler): Consider instead re-enqueing *all* messages
  // through recover(). What are the performance implications of
  // the additional queueing delay and the accumulated backlog
  // of messages post-recovery?
  if (!recovered.get().isReady()) {
    VLOG(1) << "Dropping '" << event.message->name << "' message since "
            << "not recovered yet";
    ++metrics.dropped_messages;
    return;
  }

  // Throttle the message if it's a framework message and a
  // RateLimiter is configured for the framework's principal.
  // The framework is throttled by the default RateLimiter if:
  // 1) the default RateLimiter is configured (and)
  // 2) the framework doesn't have a principal or its principal is
  //    not specified in 'flags.rate_limits'.
  // The framework is not throttled if:
  // 1) the default RateLimiter is not configured to handle case 2)
  //    above. (or)
  // 2) the principal exists in RateLimits but 'qps' is not set.
  if (principal.isSome() &&
      limiters.contains(principal.get()) &&
      limiters[principal.get()].isSome()) {
    const Owned<BoundedRateLimiter>& limiter = limiters[principal.get()].get();

    if (limiter->capacity.isNone() ||
        limiter->messages < limiter->capacity.get()) {
      limiter->messages++;
      limiter->limiter->acquire()
        .onReady(defer(self(), &Self::throttled, event, principal));
    } else {
      exceededCapacity(
          event,
          principal,
          limiter->capacity.get());
    }
  } else if ((principal.isNone() || !limiters.contains(principal.get())) &&
             isRegisteredFramework &&
             defaultLimiter.isSome()) {
    if (defaultLimiter.get()->capacity.isNone() ||
        defaultLimiter.get()->messages < defaultLimiter.get()->capacity.get()) {
      defaultLimiter.get()->messages++;
      defaultLimiter.get()->limiter->acquire()
        .onReady(defer(self(), &Self::throttled, event, None()));
    } else {
      exceededCapacity(
          event,
          principal,
          defaultLimiter.get()->capacity.get());
    }
  } else {
    _visit(event);
  }
}


void Master::visit(const ExitedEvent& event)
{
  // See comments in 'visit(const MessageEvent& event)' for which
  // RateLimiter is used to throttle this UPID and when it is not
  // throttled.
  // Note that throttling ExitedEvent is necessary so the order
  // between MessageEvents and ExitedEvents from the same PID is
  // maintained. Also ExitedEvents are not subject to the capacity.
  bool isRegisteredFramework = frameworks.principals.contains(event.pid);
  const Option<string> principal = isRegisteredFramework
    ? frameworks.principals[event.pid]
    : Option<string>::none();

  // Necessary to disambiguate below.
  typedef void(Self::*F)(const ExitedEvent&);

  if (principal.isSome() &&
      limiters.contains(principal.get()) &&
      limiters[principal.get()].isSome()) {
    limiters[principal.get()].get()->limiter->acquire()
      .onReady(defer(self(), static_cast<F>(&Self::_visit), event));
  } else if ((principal.isNone() || !limiters.contains(principal.get())) &&
             isRegisteredFramework &&
             defaultLimiter.isSome()) {
    defaultLimiter.get()->limiter->acquire()
      .onReady(defer(self(), static_cast<F>(&Self::_visit), event));
  } else {
    _visit(event);
  }
}


void Master::throttled(
    const MessageEvent& event,
    const Option<std::string>& principal)
{
  // We already know a RateLimiter is used to throttle this event so
  // here we only need to determine which.
  if (principal.isSome()) {
    CHECK_SOME(limiters[principal.get()]);
    limiters[principal.get()].get()->messages--;
  } else {
    CHECK_SOME(defaultLimiter);
    defaultLimiter.get()->messages--;
  }

  _visit(event);
}


void Master::_visit(const MessageEvent& event)
{
  // Obtain the principal before processing the Message because the
  // mapping may be deleted in handling 'UnregisterFrameworkMessage'
  // but its counter still needs to be incremented for this message.
  const Option<string> principal =
    frameworks.principals.contains(event.message->from)
      ? frameworks.principals[event.message->from]
      : Option<string>::none();

  ProtobufProcess<Master>::visit(event);

  // Increment 'messages_processed' counter if it still exists.
  // Note that it could be removed in handling
  // 'UnregisterFrameworkMessage' if it's the last framework with
  // this principal.
  if (principal.isSome() && metrics.frameworks.contains(principal.get())) {
    Counter messages_processed =
      metrics.frameworks.get(principal.get()).get()->messages_processed;
    ++messages_processed;
  }
}


void Master::exceededCapacity(
    const MessageEvent& event,
    const Option<string>& principal,
    uint64_t capacity)
{
  LOG(WARNING) << "Dropping message " << event.message->name << " from "
               << event.message->from
               << (principal.isSome() ? "(" + principal.get() + ")" : "")
               << ": capacity(" << capacity << ") exceeded";

  // Send an error to the framework which will abort the scheduler
  // driver.
  // NOTE: The scheduler driver will send back a
  // DeactivateFrameworkMessage which may be dropped as well but this
  // should be fine because the scheduler is already informed of an
  // unrecoverable error and should take action to recover.
  FrameworkErrorMessage message;
  message.set_message(
      "Message " + event.message->name +
      " dropped: capacity(" + stringify(capacity) + ") exceeded");
  send(event.message->from, message);
}


void Master::_visit(const ExitedEvent& event)
{
  Process<Master>::visit(event);
}


void fail(const string& message, const string& failure)
{
  LOG(FATAL) << message << ": " << failure;
}


Future<Nothing> Master::recover()
{
  if (!elected()) {
    return Failure("Not elected as leading master");
  }

  if (recovered.isNone()) {
    LOG(INFO) << "Recovering from registrar";

    recovered = registrar->recover(info_)
      .then(defer(self(), &Self::_recover, lambda::_1));
  }

  return recovered.get();
}


Future<Nothing> Master::_recover(const Registry& registry)
{
  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    slaves.recovered.insert(slave.info().id());
  }

  // Set up a timeout for slaves to re-register. This timeout is based
  // on the maximum amount of time the SlaveObserver allows slaves to
  // not respond to health checks.
  // TODO(bmahler): Consider making this configurable.
  slaves.recoveredTimer =
    delay(flags.slave_reregister_timeout,
          self(),
          &Self::recoveredSlavesTimeout,
          registry);

  // Recovery is now complete!
  LOG(INFO) << "Recovered " << registry.slaves().slaves().size() << " slaves"
            << " from the Registry (" << Bytes(registry.ByteSize()) << ")"
            << " ; allowing " << flags.slave_reregister_timeout
            << " for slaves to re-register";

  return Nothing();
}


void Master::recoveredSlavesTimeout(const Registry& registry)
{
  CHECK(elected());

  // TODO(bmahler): Add a 'Percentage' abstraction.
  Try<double> limit_ = numify<double>(
      strings::remove(
          flags.recovery_slave_removal_limit,
          "%",
          strings::SUFFIX));

  CHECK_SOME(limit_);

  double limit = limit_.get() / 100.0;

  // Compute the percentage of slaves to be removed, if it exceeds the
  // safety-net limit, bail!
  double removalPercentage =
    (1.0 * slaves.recovered.size()) /
    (1.0 * registry.slaves().slaves().size());

  if (removalPercentage > limit) {
    EXIT(1) << "Post-recovery slave removal limit exceeded! After "
            << SLAVE_PING_TIMEOUT * MAX_SLAVE_PING_TIMEOUTS
            << " there were " << slaves.recovered.size()
            << " (" << removalPercentage * 100 << "%) slaves recovered from the"
            << " registry that did not re-register: \n"
            << stringify(slaves.recovered) << "\n "
            << " The configured removal limit is " << limit * 100 << "%. Please"
            << " investigate or increase this limit to proceed further";
  }

  foreach (const Registry::Slave& slave, registry.slaves().slaves()) {
    if (!slaves.recovered.contains(slave.info().id())) {
      continue; // Slave re-registered.
    }

    LOG(WARNING) << "Slave " << slave.info().id()
                 << " (" << slave.info().hostname() << ") did not re-register "
                 << "within the timeout; removing it from the registrar";

    ++metrics.recovery_slave_removals;

    slaves.recovered.erase(slave.info().id());

    if (flags.registry_strict) {
      slaves.removing.insert(slave.info().id());

      registrar->apply(Owned<Operation>(new RemoveSlave(slave.info())))
        .onAny(defer(self(),
                     &Self::_removeSlave,
                     slave.info(),
                     vector<StatusUpdate>(), // No TASK_LOST updates to send.
                     lambda::_1));
    } else {
      // When a non-strict registry is in use, we want to ensure the
      // registry is used in a write-only manner. Therefore we remove
      // the slave from the registry but we do not inform the
      // framework.
      const string& message =
        "Failed to remove slave " + stringify(slave.info().id());

      registrar->apply(Owned<Operation>(new RemoveSlave(slave.info())))
        .onFailed(lambda::bind(fail, message, lambda::_1));
    }
  }
}


void Master::fileAttached(const Future<Nothing>& result, const string& path)
{
  if (result.isReady()) {
    LOG(INFO) << "Successfully attached file '" << path << "'";
  } else {
    LOG(ERROR) << "Failed to attach file '" << path << "': "
               << (result.isFailed() ? result.failure() : "discarded");
  }
}


void Master::submitScheduler(const string& name)
{
  LOG(INFO) << "Scheduler submit request for " << name;
  SubmitSchedulerResponse response;
  response.set_okay(false);
  reply(response);
}


void Master::contended(const Future<Future<Nothing>>& candidacy)
{
  CHECK(!candidacy.isDiscarded());

  if (candidacy.isFailed()) {
    EXIT(1) << "Failed to contend: " << candidacy.failure();
  }

  // Watch for candidacy change.
  candidacy.get()
    .onAny(defer(self(), &Master::lostCandidacy, lambda::_1));
}


void Master::lostCandidacy(const Future<Nothing>& lost)
{
  CHECK(!lost.isDiscarded());

  if (lost.isFailed()) {
    EXIT(1) << "Failed to watch for candidacy: " << lost.failure();
  }

  if (elected()) {
    EXIT(1) << "Lost leadership... committing suicide!";
  }

  LOG(INFO) << "Lost candidacy as a follower... Contend again";
  contender->contend()
    .onAny(defer(self(), &Master::contended, lambda::_1));
}


void Master::detected(const Future<Option<MasterInfo>>& _leader)
{
  CHECK(!_leader.isDiscarded());

  if (_leader.isFailed()) {
    EXIT(1) << "Failed to detect the leading master: " << _leader.failure()
            << "; committing suicide!";
  }

  bool wasElected = elected();
  leader = _leader.get();

  LOG(INFO) << "The newly elected leader is "
            << (leader.isSome()
                ? (leader.get().pid() + " with id " + leader.get().id())
                : "None");

  if (wasElected && !elected()) {
    EXIT(1) << "Lost leadership... committing suicide!";
  }

  if (elected()) {
    electedTime = Clock::now();

    if (!wasElected) {
      LOG(INFO) << "Elected as the leading master!";

      // Begin the recovery process, bail if it fails or is discarded.
      recover()
        .onFailed(lambda::bind(fail, "Recovery failed", lambda::_1))
        .onDiscarded(lambda::bind(fail, "Recovery failed", "discarded"));
    } else {
      // This happens if there is a ZK blip that causes a re-election
      // but the same leading master is elected as leader.
      LOG(INFO) << "Re-elected as the leading master";
    }
  }

  // Keep detecting.
  detector->detect(leader)
    .onAny(defer(self(), &Master::detected, lambda::_1));
}


// Helper to convert authorization result to Future<Option<Error> >.
static Future<Option<Error>> _authorize(const string& message, bool authorized)
{
  if (authorized) {
    return None();
  }

  return Error(message);
}


Future<Option<Error>> Master::validate(
    const FrameworkInfo& frameworkInfo,
    const UPID& from)
{
  if (flags.authenticate_frameworks) {
    if (!authenticated.contains(from)) {
      // This could happen if another authentication request came
      // through before we are here or if a framework tried to
      // (re-)register without authentication.
      return Error("Framework at " + stringify(from) + " is not authenticated");
    } else if (frameworkInfo.has_principal() &&
               frameworkInfo.principal() != authenticated[from]) {
      return Error(
          "Framework principal '" + frameworkInfo.principal() +
          "' does not match authenticated principal '" + authenticated[from]  +
          "'");
    } else if (!frameworkInfo.has_principal()) {
      // We allow an authenticated framework to not specify a
      // principal in FrameworkInfo but we'd prefer if it did so we log
      // a WARNING here when this happens.
      LOG(WARNING) << "Framework at " << from << " (authenticated as '"
                   << authenticated[from]
                   << "') does not specify principal in its FrameworkInfo";
    }
  }

  // TODO(vinod): Deprecate this in favor of ACLs.
  if (!roles.contains(frameworkInfo.role())) {
    return Error("Role '" + frameworkInfo.role() + "' is invalid");
  }

  if (authorizer.isNone()) {
    // Authorization is disabled.
    return None();
  }

  LOG(INFO)
    << "Authorizing framework principal '" << frameworkInfo.principal()
    << "' to receive offers for role '" << frameworkInfo.role() << "'";

  mesos::ACL::RegisterFramework request;
  if (frameworkInfo.has_principal()) {
    request.mutable_principals()->add_values(frameworkInfo.principal());
  } else {
    // Framework doesn't have a principal set.
    request.mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  }
  request.mutable_roles()->add_values(frameworkInfo.role());

  return authorizer.get()->authorize(request).then(
      lambda::bind(&_authorize,
                   "Not authorized to use role '" + frameworkInfo.role() + "'",
                   lambda::_1));
}


void Master::registerFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo)
{
  ++metrics.messages_register_framework;

  if (authenticating.contains(from)) {
    // TODO(vinod): Consider dropping this request and fix the tests
    // to deal with the drop. Currently there is a race between master
    // realizing the framework is authenticated and framework sending
    // a registration request. Dropping this message will cause the
    // framework to retry slowing down the tests.
    LOG(INFO) << "Queuing up registration request for"
              << " framework '" << frameworkInfo.name() << "' at " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(), &Self::registerFramework, from, frameworkInfo));
    return;
  }

  LOG(INFO) << "Received registration request for"
            << " framework '" << frameworkInfo.name() << "' at " << from;

  validate(frameworkInfo, from)
    .onAny(defer(self(),
                 &Master::_registerFramework,
                 from,
                 frameworkInfo,
                 lambda::_1));
}


void Master::_registerFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    const Future<Option<Error>>& validationError)
{
  CHECK_READY(validationError);
  if (validationError.get().isSome()) {
    LOG(INFO) << "Refusing registration of framework '"
              << frameworkInfo.name() << "' at " << from
              << ": " << validationError.get().get().message;

    FrameworkErrorMessage message;
    message.set_message(validationError.get().get().message);
    send(from, message);
    return;
  }

  if (authenticating.contains(from)) {
    // This could happen if a new authentication request came from the
    // same framework while validation was in progress.
    LOG(INFO) << "Dropping registration request for framework"
              << " '" << frameworkInfo.name() << "' at " << from
              << " because new authentication attempt is in progress";

    return;
  }

  if (flags.authenticate_frameworks && !authenticated.contains(from)) {
    // This could happen if another (failed over) framework
    // authenticated while validation was in progress.
    LOG(INFO)
      << "Dropping registration request for framework '" << frameworkInfo.name()
      << "' at " << from << " because it is not authenticated";
    return;
  }

  // Check if this framework is already registered (because it retries).
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->pid == from) {
      LOG(INFO) << "Framework " << *framework
                << " already registered, resending acknowledgement";
      FrameworkRegisteredMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.mutable_master_info()->MergeFrom(info_);
      send(from, message);
      return;
    }
  }

  Framework* framework =
    new Framework(frameworkInfo, newFrameworkId(), from, Clock::now());

  LOG(INFO) << "Registering framework " << *framework;

  // TODO(vinod): Deprecate this in favor of authorization.
  bool rootSubmissions = flags.root_submissions;

  if (framework->info.user() == "root" && rootSubmissions == false) {
    LOG(INFO) << "Framework " << *framework << " registering as root, but "
              << "root submissions are disabled on this cluster";
    FrameworkErrorMessage message;
    message.set_message("User 'root' is not allowed to run frameworks");
    send(from, message);
    delete framework;
    return;
  }

  addFramework(framework);

  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.mutable_master_info()->MergeFrom(info_);
  send(framework->pid, message);
}


void Master::reregisterFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    bool failover)
{
  ++metrics.messages_reregister_framework;

  if (!frameworkInfo.has_id() || frameworkInfo.id() == "") {
    LOG(ERROR) << "Framework '" << frameworkInfo.name() << "' at " << from
               << " re-registering without an id!";
    FrameworkErrorMessage message;
    message.set_message("Framework reregistering without a framework id");
    send(from, message);
    return;
  }

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up re-registration request for framework "
              << frameworkInfo.id() << " (" << frameworkInfo.name() << ") at "
              << from << " because authentication is still in progress";

    // TODO(vinod): Consider dropping this request and fix the tests
    // to deal with the drop. See 'Master::registerFramework()' for
    // more details.
    authenticating[from]
      .onReady(defer(self(),
                     &Self::reregisterFramework,
                     from,
                     frameworkInfo,
                     failover));
    return;
  }

  foreach (const shared_ptr<Framework>& framework, frameworks.completed) {
    if (framework->id == frameworkInfo.id()) {
      // This could happen if a framework tries to re-register after
      // its failover timeout has elapsed or it unregistered itself
      // by calling 'stop()' on the scheduler driver.
      // TODO(vinod): Master should persist admitted frameworks to the
      // registry and remove them from it after failover timeout.
      LOG(WARNING) << "Completed framework " << *framework
                   << " attempted to re-register";
      FrameworkErrorMessage message;
      message.set_message("Completed framework attempted to re-register");
      send(from, message);
      return;
    }
  }

  LOG(INFO) << "Received re-registration request from framework "
            << frameworkInfo.id() << " (" << frameworkInfo.name()
            << ") at " << from;

  validate(frameworkInfo, from)
    .onAny(defer(self(),
                 &Master::_reregisterFramework,
                 from,
                 frameworkInfo,
                 failover,
                 lambda::_1));
}


void Master::_reregisterFramework(
    const UPID& from,
    const FrameworkInfo& frameworkInfo,
    bool failover,
    const Future<Option<Error>>& validationError)
{
  CHECK_READY(validationError);
  if (validationError.get().isSome()) {
    LOG(INFO) << "Refusing re-registration of framework " << frameworkInfo.id()
              << " (" << frameworkInfo.name() << ") " << " at " << from
              << ": " << validationError.get().get().message;

    FrameworkErrorMessage message;
    message.set_message(validationError.get().get().message);
    send(from, message);
    return;
  }

  if (authenticating.contains(from)) {
    // This could happen if a new authentication request came from the
    // same framework while validation was in progress.
    LOG(INFO) << "Dropping re-registration request of framework "
              << frameworkInfo.id() << " (" << frameworkInfo.name() << ") at "
              << from << " because new authentication attempt is in progress";
    return;
  }

  if (flags.authenticate_frameworks && !authenticated.contains(from)) {
    // This could happen if another (failed over) framework
    // authenticated while validation was in progress. It is important
    // to drop this because if this request is from a failing over
    // framework (pid = from) we don't want to failover the already
    // registered framework (pid = framework->pid).
    LOG(INFO) << "Dropping re-registration request of framework "
              << frameworkInfo.id() << " (" << frameworkInfo.name() << ") "
              << " at " << from << " because it is not authenticated";
    return;
  }

  LOG(INFO) << "Re-registering framework " << frameworkInfo.id()
            << " (" << frameworkInfo.name() << ") " << " at " << from;

  if (frameworks.registered.count(frameworkInfo.id()) > 0) {
    // Using the "failover" of the scheduler allows us to keep a
    // scheduler that got partitioned but didn't die (in ZooKeeper
    // speak this means didn't lose their session) and then
    // eventually tried to connect to this master even though
    // another instance of their scheduler has reconnected. This
    // might not be an issue in the future when the
    // master/allocator launches the scheduler can get restarted
    // (if necessary) by the master and the master will always
    // know which scheduler is the correct one.

    Framework* framework =
      CHECK_NOTNULL(frameworks.registered[frameworkInfo.id()]);

    framework->reregisteredTime = Clock::now();

    if (failover) {
      // We do not attempt to detect a duplicate re-registration
      // message here because it is impossible to distinguish between
      // a duplicate message, and a scheduler failover to the same
      // pid, given the existing libprocess primitives (PID does not
      // identify the libprocess Process instance).

      // TODO(benh): Should we check whether the new scheduler has
      // given us a different framework name, user name or executor
      // info?
      LOG(INFO) << "Framework " << *framework << " failed over";
      failoverFramework(framework, from);
    } else if (from != framework->pid) {
      LOG(ERROR)
        << "Disallowing re-registration attempt of framework " << *framework
        << " because it is not expected from " << from;
      FrameworkErrorMessage message;
      message.set_message("Framework failed over");
      send(from, message);
      return;
    } else {
      LOG(INFO) << "Allowing framework " << *framework
                << " to re-register with an already used id";

      // Remove any offers sent to this framework.
      // NOTE: We need to do this because the scheduler might have
      // replied to the offers but the driver might have dropped
      // those messages since it wasn't connected to the master.
      foreach (Offer* offer, utils::copy(framework->offers)) {
        allocator->recoverResources(
            offer->framework_id(),
            offer->slave_id(),
            offer->resources(),
            None());
        removeOffer(offer, true); // Rescind.
      }

      framework->connected = true;

      // Reactivate the framework.
      // NOTE: We do this after recovering resources (above) so that
      // the allocator has the correct view of the framework's share.
      if (!framework->active) {
        framework->active = true;
        allocator->activateFramework(framework->id);
      }

      FrameworkReregisteredMessage message;
      message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
      message.mutable_master_info()->MergeFrom(info_);
      send(from, message);
      return;
    }
  } else {
    // We don't have a framework with this ID, so we must be a newly
    // elected Mesos master to which either an existing scheduler or a
    // failed-over one is connecting. Create a Framework object and add
    // any tasks it has that have been reported by reconnecting slaves.
    Framework* framework =
      new Framework(frameworkInfo, frameworkInfo.id(), from, Clock::now());
    framework->reregisteredTime = Clock::now();

    // TODO(benh): Check for root submissions like above!

    // Add active tasks and executors to the framework.
    foreachvalue (Slave* slave, slaves.registered) {
      foreachvalue (Task* task, slave->tasks[framework->id]) {
        framework->addTask(task);
      }
      foreachvalue (const ExecutorInfo& executor,
                    slave->executors[framework->id]) {
        framework->addExecutor(slave->id, executor);
      }
    }

    // N.B. Need to add the framework _after_ we add its tasks
    // (above) so that we can properly determine the resources it's
    // currently using!
    addFramework(framework);

    // TODO(bmahler): We have to send a registered message here for
    // the re-registering framework, per the API contract. Send
    // re-register here per MESOS-786; requires deprecation or it
    // will break frameworks.
    FrameworkRegisteredMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    message.mutable_master_info()->MergeFrom(info_);
    send(framework->pid, message);
  }

  CHECK(frameworks.registered.contains(frameworkInfo.id()))
    << "Unknown framework " << frameworkInfo.id()
    << " (" << frameworkInfo.name() << ")";

  // Broadcast the new framework pid to all the slaves. We have to
  // broadcast because an executor might be running on a slave but
  // it currently isn't running any tasks. This could be a
  // potential scalability issue ...
  foreachvalue (Slave* slave, slaves.registered) {
    UpdateFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkInfo.id());
    message.set_pid(from);
    send(slave->pid, message);
  }

  return;
}


void Master::unregisterFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  ++metrics.messages_unregister_framework;

  LOG(INFO) << "Asked to unregister framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) {
    if (framework->pid == from) {
      removeFramework(framework);
    } else {
      LOG(WARNING)
        << "Ignoring unregister framework message for framework " << *framework
        << " because it is not expected from " << from;
    }
  }
}


void Master::deactivateFramework(
    const UPID& from,
    const FrameworkID& frameworkId)
{
  ++metrics.messages_deactivate_framework;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring deactivate framework message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  deactivate(framework);
}


void Master::disconnect(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Disconnecting framework " << *framework;

  framework->connected = false;

  // Remove the framework from authenticated. This is safe because
  // a framework will always reauthenticate before (re-)registering.
  authenticated.erase(framework->pid);

  deactivate(framework);
}


void Master::deactivate(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Deactivating framework " << *framework;

  // Stop sending offers here for now.
  framework->active = false;

  // Tell the allocator to stop allocating resources to this framework.
  allocator->deactivateFramework(framework->id);

  // Remove the framework's offers.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());
    removeOffer(offer, true); // Rescind.
  }
}


void Master::disconnect(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Disconnecting slave " << *slave;

  slave->connected = false;

  // Inform the slave observer.
  dispatch(slave->observer, &SlaveObserver::disconnect);

  // Remove the slave from authenticated. This is safe because
  // a slave will always reauthenticate before (re-)registering.
  authenticated.erase(slave->pid);

  deactivate(slave);
}


void Master::deactivate(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Deactivating slave " << *slave;

  slave->active = false;

  allocator->deactivateSlave(slave->id);

  // Remove and rescind offers.
  foreach (Offer* offer, utils::copy(slave->offers)) {
    allocator->recoverResources(
        offer->framework_id(), slave->id, offer->resources(), None());

    removeOffer(offer, true); // Rescind!
  }
}


void Master::resourceRequest(
    const UPID& from,
    const FrameworkID& frameworkId,
    const vector<Request>& requests)
{
  ++metrics.messages_resource_request;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring resource request message from framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring resource request message from framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  LOG(INFO) << "Requesting resources for framework " << *framework;
  allocator->requestResources(frameworkId, requests);
}


// Abstraction for performing any validations, aggregations, etc. of
// tasks that a framework attempts to run within the resources
// provided by offers. A validator can return an optional error which
// will cause the master to send a failed status update back to the
// framework for only that task. An instance will be reused for each
// task from same 'launchTasks()', but not for task from different
// offers.
struct TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources) = 0;

  virtual ~TaskInfoValidator() {}
};


// Validates that a task id is valid, i.e., contains only valid
// characters.
struct TaskIDValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    const string& id = task.task_id().value();

    if (std::count_if(id.begin(), id.end(), invalid) > 0) {
      return Error("TaskID '" + id + "' contains invalid characters");
    }

    return None();
  }

  static bool invalid(char c)
  {
    return iscntrl(c) || c == '/' || c == '\\';
  }
};


// Validates that the slave ID used by a task is correct.
struct SlaveIDValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    if (!(task.slave_id() == slave.id)) {
      return Error(
          "Task uses invalid slave " + task.slave_id().value() +
          " while slave " + slave.id.value() + " is expected");
    }

    return None();
  }
};


// Validates that each task uses a unique ID. Regardless of whether a
// task actually gets launched (for example, another validator may
// return an error for a task), we always consider it an error when a
// task tries to re-use an ID.
struct UniqueTaskIDValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    const TaskID& taskId = task.task_id();

    if (framework.tasks.contains(taskId)) {
      return Error("Task has duplicate ID: " + taskId.value());
    }

    return None();
  }
};


// Validates that resources specified by the framework are valid.
struct ResourceValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    // This is used to ensure no duplicated persistence id exists.
    // TODO(jieyu): The check we have right now is a partial check for
    // the current task. We need to add checks against slave's
    // existing tasks and executors as well.
    hashmap<string, hashset<string>> persistenceIds;

    Option<Error> error = Resources::validate(task.resources());
    if (error.isSome()) {
      return Error("Task uses invalid resources: " + error.get().message);
    }

    // Ensure any DiskInfos in the task are valid according to the
    // currently supported semantics.
    foreach (const Resource& resource, task.resources()) {
      if (resource.has_disk()) {
        error = validateDiskInfo(resource);
        if (error.isSome()) {
          return Error("Task uses invalid DiskInfo: " + error.get().message);
        }

        if (resource.disk().has_persistence()) {
          string role = resource.role();
          string id = resource.disk().persistence().id();

          if (persistenceIds[role].contains(id)) {
            return Error("Task uses duplicated persistence ID " + id);
          }

          persistenceIds[role].insert(id);
        }
      }
    }

    if (task.has_executor()) {
      Option<Error> error = Resources::validate(task.executor().resources());
      if (error.isSome()) {
        return Error(
            "Executor uses invalid resources: " + error.get().message);
      }

      // Ensure any DiskInfos in the executor are valid according to
      // the currently supported semantics.
      foreach (const Resource& resource, task.executor().resources()) {
        if (resource.has_disk()) {
          error = validateDiskInfo(resource);
          if (error.isSome()) {
            return Error(
                "Executor uses invalid DiskInfo: " + error.get().message);
          }

          if (resource.disk().has_persistence()) {
            string role = resource.role();
            string id = resource.disk().persistence().id();

            if (persistenceIds[role].contains(id)) {
              return Error("Executor uses duplicated persistence ID " + id);
            }

            persistenceIds[role].insert(id);
          }
        }
      }
    }

    return None();
  }

  Option<Error> validateDiskInfo(const Resource& resource)
  {
    CHECK(resource.has_disk());

    if (resource.disk().has_persistence()) {
      if (resource.role() == "*") {
        return Error("Persistent disk volume is disallowed for '*' role");
      }
      if (!resource.disk().has_volume()) {
        return Error("Persistent disk should specify a volume");
      }
      if (resource.disk().volume().mode() == Volume::RO) {
        return Error("Read-only volume is not supported for DiskInfo");
      }
      if (resource.disk().volume().has_host_path()) {
        return Error("Volume in DiskInfo should not have 'host_path' set");
      }

      // Ensure persistence ID does not have invalid characters.
      string id = resource.disk().persistence().id();
      if (std::count_if(id.begin(), id.end(), invalid) > 0) {
        return Error("Persistence ID '" + id + "' contains invalid characters");
      }
    } else if (resource.disk().has_volume()) {
      return Error("Non-persistent disk volume is not supported");
    } else {
      return Error("DiskInfo is set but empty");
    }

    return None();
  }

  static bool invalid(char c)
  {
    return iscntrl(c) || c == '/' || c == '\\';
  }
};


// Validates that the task and the executor are using proper amount of
// resources. For instance, the used resources by a task on each slave
// should not exceed the total resources offered on that slave.
struct ResourceUsageValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    Resources taskResources = task.resources();

    if (taskResources.empty()) {
      return Error("Task uses no resources");
    }

    Resources executorResources;
    if (task.has_executor()) {
      executorResources = task.executor().resources();
    }

    // Validate minimal cpus and memory resources of executor and log
    // warnings if not set.
    if (task.has_executor()) {
      // TODO(martin): MESOS-1807. Return Error instead of logging a
      // warning in 0.22.0.
      Option<double> cpus =  executorResources.cpus();
      if (cpus.isNone() || cpus.get() < MIN_CPUS) {
        LOG(WARNING)
          << "Executor " << stringify(task.executor().executor_id())
          << " for task " << stringify(task.task_id())
          << " uses less CPUs ("
          << (cpus.isSome() ? stringify(cpus.get()) : "None")
          << ") than the minimum required (" << MIN_CPUS
          << "). Please update your executor, as this will be mandatory "
          << "in future releases.";
      }

      Option<Bytes> mem = executorResources.mem();
      if (mem.isNone() || mem.get() < MIN_MEM) {
        LOG(WARNING)
          << "Executor " << stringify(task.executor().executor_id())
          << " for task " << stringify(task.task_id())
          << " uses less memory ("
          << (mem.isSome() ? stringify(mem.get().megabytes()) : "None")
          << ") than the minimum required (" << MIN_MEM
          << "). Please update your executor, as this will be mandatory "
          << "in future releases.";
      }
    }

    // Validate if resources needed by the task (and its executor in
    // case the executor is new) are available.
    Resources resources = taskResources;
    if (!slave.hasExecutor(framework.id, task.executor().executor_id())) {
      resources += executorResources;
    }

    // We allow frameworks to implicitly acquire persistent disks
    // through task and executor resources. This means that we need to
    // infer these implicit disk acquisition transformations on the
    // offered resources, so that we can validate resource usage.
    //
    // NOTE: ResourceValidator ensures that there are no duplicate
    // persistence IDs per role in 'resources'.
    //
    // NOTE: 'offeredResources' will not contain duplicate persistence
    // IDs per role, given we do not construct such offers.
    Resources::CompositeTransformation transformation;
    foreach (const Resource& disk, resources.persistentDisks()) {
      if (!offeredResources.contains(disk)) {
        // This is an implicit acquisition. The framework is not
        // allowed to mutate an offered persistent disk, so we need to
        // check the offered resources for this persistence ID within
        // the role.
        //
        // TODO(jieyu): We need to ensure this persistence ID within
        // the role does not clash with any in-use persistent disks on
        // the slave.
        string id = disk.disk().persistence().id();
        foreach (const Resource& offered, offeredResources.persistentDisks()) {
          if (offered.role() == disk.role() &&
              offered.disk().persistence().id() == id) {
            return Error("Duplicated persistence ID '" + id + "'");
          }
        }

        transformation.add(Resources::AcquirePersistentDisk(disk));
      }
    }

    // Validate that the offered resources are sufficient for
    // launching this task/executor. To do that, we must first apply
    // the implicit transformations.
    Try<Resources> transformedOfferedResources =
      transformation(offeredResources);

    if (transformedOfferedResources.isError()) {
      // TODO(jieyu): Revisit this error message once we start to
      // allow other types of transformations (e.g., dynamic
      // reservations).
      return Error(
          "Failed to acquire persistent disks: " +
          transformedOfferedResources.error());
    }

    if (!transformedOfferedResources.get()
          .contains(resources + usedResources)) {
      return Error(
          "Task uses more resources " + stringify(resources) +
          " than available " +
          stringify(transformedOfferedResources.get() - usedResources));
    }

    return None();
  }
};


// Validates that tasks that use the "same" executor (i.e., same
// ExecutorID) have an identical ExecutorInfo.
struct ExecutorInfoValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    if (task.has_executor() == task.has_command()) {
      return Error(
          "Task should have at least one (but not both) of CommandInfo or "
          "ExecutorInfo present");
    }

    if (task.has_executor()) {
      // The master currently expects ExecutorInfo.framework_id to be
      // set even though it is an optional field. Currently, the
      // scheduler driver ensures that the field is set. For
      // schedulers not using the driver, we need to do the validation
      // here.
      // TODO(bmahler): Set this field in the master instead of
      // depending on the scheduler driver do it.
      if (!task.executor().has_framework_id()) {
        return Error(
            "Task has invalid ExecutorInfo: missing field 'framework_id'");
      }

      if (!(task.executor().framework_id() == framework.id)) {
        return Error(
            "ExecutorInfo has an invalid FrameworkID"
            " (Actual: " + stringify(task.executor().framework_id()) +
            " vs Expected: " + stringify(framework.id) + ")");
      }

      const ExecutorID& executorId = task.executor().executor_id();
      Option<ExecutorInfo> executorInfo = None();

      if (slave.hasExecutor(framework.id, executorId)) {
        executorInfo = slave.executors.get(framework.id).get().get(executorId);
      }

      if (executorInfo.isSome() && !(task.executor() == executorInfo.get())) {
        return Error(
            "Task has invalid ExecutorInfo (existing ExecutorInfo"
            " with same ExecutorID is not compatible).\n"
            "------------------------------------------------------------\n"
            "Existing ExecutorInfo:\n" +
            stringify(executorInfo.get()) + "\n"
            "------------------------------------------------------------\n"
            "Task's ExecutorInfo:\n" +
            stringify(task.executor()) + "\n"
            "------------------------------------------------------------\n");
      }
    }

    return None();
  }
};


// Validates that a task that asks for checkpointing is not being
// launched on a slave that has not enabled checkpointing.
struct CheckpointValidator : TaskInfoValidator
{
  virtual Option<Error> operator () (
      const TaskInfo& task,
      const Framework& framework,
      const Slave& slave,
      const Resources& offeredResources,
      const Resources& usedResources)
  {
    if (framework.info.checkpoint() && !slave.info.checkpoint()) {
      return Error(
          "Task asked to be checkpointed but slave " +
          stringify(slave.id) + " has checkpointing disabled");
    }

    return None();
  }
};


// OfferValidators are similar to the TaskInfoValidator pattern and
// are used for validation and aggregation of offers. The error
// reporting scheme is also similar to TaskInfoValidator. However,
// offer processing (and subsequent task processing) is aborted
// altogether if offer validator reports an error.
struct OfferValidator
{
  virtual Option<Error> operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master) = 0;

  virtual ~OfferValidator() {}

  Slave* getSlave(Master* master, const SlaveID& slaveId)
  {
    CHECK_NOTNULL(master);
    return master->getSlave(slaveId);
  }

  Offer* getOffer(Master* master, const OfferID& offerId)
  {
    CHECK_NOTNULL(master);
    return master->getOffer(offerId);
  }
};


// Validates the validity/liveness of an offer.
struct ValidOfferValidator : OfferValidator
{
  virtual Option<Error> operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    return None();
  }
};


// Validates that an offer belongs to the expected framework.
struct FrameworkValidator : OfferValidator
{
  virtual Option<Error> operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    if (!(framework.id == offer->framework_id())) {
      return Error(
          "Offer " + stringify(offer->id()) +
          " has invalid framework " + stringify(offer->framework_id()) +
          " while framework " + stringify(framework.id) + " is expected");
    }

    return None();
  }
};


// Validates that the slave is valid and ensures that all offers
// belong to the same slave.
struct SlaveValidator : OfferValidator
{
  virtual Option<Error> operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    Offer* offer = getOffer(master, offerId);
    if (offer == NULL) {
      return Error("Offer " + stringify(offerId) + " is no longer valid");
    }

    Slave* slave = getSlave(master, offer->slave_id());

    // This is not possible because the offer should've been removed.
    CHECK(slave != NULL)
      << "Offer " << offerId
      << " outlived slave " << offer->slave_id();

    // This is not possible because the offer should've been removed.
    CHECK(slave->connected)
      << "Offer " << offerId
      << " outlived disconnected slave " << *slave;

    if (slaveId.isNone()) {
      // Set slave id and use as base case for validation.
      slaveId = slave->id;
    } else if (!(slave->id == slaveId.get())) {
      return Error(
          "Aggregated offers must belong to one single slave. Offer " +
          stringify(offerId) + " uses slave " +
          stringify(slave->id) + " and slave " +
          stringify(slaveId.get()));
    }

    return None();
  }

  Option<const SlaveID> slaveId;
};


// Validates that an offer only appears once in offer list.
struct UniqueOfferIDValidator : OfferValidator
{
  virtual Option<Error> operator () (
      const OfferID& offerId,
      const Framework& framework,
      Master* master)
  {
    if (offers.contains(offerId)) {
      return Error("Duplicate offer " + stringify(offerId) + " in offer list");
    }
    offers.insert(offerId);

    return None();
  }

  hashset<OfferID> offers;
};


void Master::launchTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const vector<TaskInfo>& tasks,
    const Filters& filters,
    const vector<OfferID>& offerIds)
{
  if (!tasks.empty()) {
    ++metrics.messages_launch_tasks;
  } else {
    ++metrics.messages_decline_offers;
  }

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offers " << stringify(offerIds)
      << " of framework " << frameworkId
      << " because the framework cannot be found";

    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring launch tasks message for offers " << stringify(offerIds)
      << " of framework " << frameworkId << " from '" << from
      << "' because it is not from the registered framework '"
      << framework->pid << "'";

    return;
  }

  // TODO(bmahler): We currently only support using multiple offers
  // for a single slave.
  Resources offeredResources;
  Option<SlaveID> slaveId = None();
  Option<Error> error = None();

  if (offerIds.empty()) {
    error = Error("No offers specified");
  } else {
    vector<Owned<OfferValidator>> offerValidators = {
      Owned<OfferValidator>(new ValidOfferValidator()),
      Owned<OfferValidator>(new FrameworkValidator()),
      Owned<OfferValidator>(new SlaveValidator()),
      Owned<OfferValidator>(new UniqueOfferIDValidator())
    };

    // Validate the offers.
    foreach (const OfferID& offerId, offerIds) {
      foreach (const Owned<OfferValidator>& validator, offerValidators) {
        if (error.isNone()) {
          error = (*validator)(offerId, *framework, this);
        }
      }
    }

    // Compute offered resources and remove the offers. If the
    // validation failed, return resources to the allocator.
    foreach (const OfferID& offerId, offerIds) {
      Offer* offer = getOffer(offerId);
      if (offer != NULL) {
        slaveId = offer->slave_id();
        offeredResources += offer->resources();

        if (error.isSome()) {
          allocator->recoverResources(
              offer->framework_id(),
              offer->slave_id(),
              offer->resources(),
              None());
        }
        removeOffer(offer);
      }
    }
  }

  // If invalid, send TASK_LOST for the launch attempts.
  if (error.isSome()) {
    LOG(WARNING) << "Launch tasks message used invalid offers '"
                 << stringify(offerIds) << "': " << error.get().message;

    foreach (const TaskInfo& task, tasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          "Task launched with invalid offers: " + error.get().message,
          TaskStatus::REASON_INVALID_OFFERS);

      metrics.tasks_lost++;
      stats.tasks[TASK_LOST]++;

      forward(update, UPID(), framework);
    }

    return;
  }

  CHECK_SOME(slaveId);
  Slave* slave = CHECK_NOTNULL(getSlave(slaveId.get()));

  LOG(INFO) << "Processing reply for offers: " << stringify(offerIds)
            << " on slave " << *slave
            << " for framework " << *framework;

  // Authorize each task. A task is in 'framework->pendingTasks'
  // before it is authorized.
  list<Future<bool>> futures;
  foreach (const TaskInfo& task, tasks) {
    futures.push_back(authorizeTask(task, framework));

    // Add to pending tasks.
    // NOTE: The task ID here hasn't been validated yet, but it
    // doesn't matter. If the task ID is not valid, the task won't be
    // launched anyway. If two tasks have the same ID, the second one
    // will not be put into 'framework->pendingTasks', therefore will
    // not be launched.
    if (!framework->pendingTasks.contains(task.task_id())) {
      framework->pendingTasks[task.task_id()] = task;
    }

    stats.tasks[TASK_STAGING]++;
  }

  // Wait for all the tasks to be authorized.
  await(futures)
    .onAny(defer(self(),
                 &Master::_launchTasks,
                 frameworkId,
                 slaveId.get(),
                 tasks,
                 offeredResources,
                 filters,
                 lambda::_1));
}


Option<Error> Master::validateTask(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave,
    const Resources& offeredResources,
    const Resources& usedResources)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);

  // Create task validators.
  // NOTE: The order in which the following validators are executed
  // does matter! For example, ResourceUsageValidator assumes that
  // ExecutorInfo is valid which is verified by ExecutorInfoValidator.
  // TODO(vinod): Create the validators on the stack and make the
  // validate operation const.
  vector<Owned<TaskInfoValidator>> taskValidators = {
    Owned<TaskInfoValidator>(new TaskIDValidator()),
    Owned<TaskInfoValidator>(new SlaveIDValidator()),
    Owned<TaskInfoValidator>(new UniqueTaskIDValidator()),
    Owned<TaskInfoValidator>(new CheckpointValidator()),
    Owned<TaskInfoValidator>(new ExecutorInfoValidator()),
    Owned<TaskInfoValidator>(new ResourceValidator()),
    Owned<TaskInfoValidator>(new ResourceUsageValidator())
  };

  // TODO(benh): Add a HealthCheckValidator.

  // TODO(jieyu): Add a CommandInfoValidator.

  // Invoke each validator.
  Option<Error> error = None();
  foreach (const Owned<TaskInfoValidator>& validator, taskValidators) {
    error = (*validator)(
        task,
        *framework,
        *slave,
        offeredResources,
        usedResources);

    if (error.isSome()) {
      break;
    }
  }

  if (error.isSome()) {
    return Error(error.get().message);
  }

  return None();
}


Future<bool> Master::authorizeTask(
    const TaskInfo& task,
    Framework* framework)
{
  if (authorizer.isNone()) {
    // Authorization is disabled.
    return true;
  }

  // Authorize the task.
  string user = framework->info.user(); // Default user.
  if (task.has_command() && task.command().has_user()) {
    user = task.command().user();
  } else if (task.has_executor() && task.executor().command().has_user()) {
    user = task.executor().command().user();
  }

  LOG(INFO)
    << "Authorizing framework principal '" << framework->info.principal()
    << "' to launch task " << task.task_id() << " as user '" << user << "'";

  mesos::ACL::RunTask request;
  if (framework->info.has_principal()) {
    request.mutable_principals()->add_values(framework->info.principal());
  } else {
    // Framework doesn't have a principal set.
    request.mutable_principals()->set_type(mesos::ACL::Entity::ANY);
  }
  request.mutable_users()->add_values(user);

  return authorizer.get()->authorize(request);
}


Resources Master::addTask(
    const TaskInfo& task,
    Framework* framework,
    Slave* slave)
{
  CHECK_NOTNULL(framework);
  CHECK_NOTNULL(slave);
  CHECK(slave->connected) << "Adding task " << task.task_id()
                          << " to disconnected slave " << *slave;

  // The resources consumed.
  Resources resources = task.resources();

  // Determine if this task launches an executor, and if so make sure
  // the slave and framework state has been updated accordingly.
  Option<ExecutorID> executorId;

  if (task.has_executor()) {
    // TODO(benh): Refactor this code into Slave::addTask.
    if (!slave->hasExecutor(framework->id, task.executor().executor_id())) {
      CHECK(!framework->hasExecutor(slave->id, task.executor().executor_id()))
        << "Executor " << task.executor().executor_id()
        << " known to the framework " << *framework
        << " but unknown to the slave " << *slave;

      slave->addExecutor(framework->id, task.executor());
      framework->addExecutor(slave->id, task.executor());

      resources += task.executor().resources();
    }

    executorId = task.executor().executor_id();
  }

  // Add the task to the framework and slave.
  Task* t = new Task();
  t->mutable_framework_id()->MergeFrom(framework->id);
  t->set_state(TASK_STAGING);
  t->set_name(task.name());
  t->mutable_task_id()->MergeFrom(task.task_id());
  t->mutable_slave_id()->MergeFrom(task.slave_id());
  t->mutable_resources()->MergeFrom(task.resources());

  if (executorId.isSome()) {
    t->mutable_executor_id()->MergeFrom(executorId.get());
  }

  t->mutable_labels()->MergeFrom(task.labels());
  if (task.has_discovery()) {
    t->mutable_discovery()->MergeFrom(task.discovery());
  }

  slave->addTask(t);
  framework->addTask(t);

  return resources;
}


void Master::_launchTasks(
    const FrameworkID& frameworkId,
    const SlaveID& slaveId,
    const vector<TaskInfo>& tasks,
    const Resources& offeredResources,
    const Filters& filters,
    const Future<list<Future<bool>>>& authorizations)
{
  CHECK_READY(authorizations);
  CHECK_EQ(authorizations.get().size(), tasks.size());

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring launch tasks message for framework " << frameworkId
      << " because the framework cannot be found";

    // Tell the allocator about the recovered resources.
    allocator->recoverResources(
        frameworkId,
        slaveId,
        offeredResources,
        None());

    return;
  }

  Slave* slave = getSlave(slaveId);
  if (slave == NULL || !slave->connected) {
    foreach (const TaskInfo& task, tasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          slave == NULL ? "Slave removed" : "Slave disconnected",
          slave == NULL ?
              TaskStatus::REASON_SLAVE_REMOVED :
              TaskStatus::REASON_SLAVE_DISCONNECTED);

      metrics.tasks_lost++;
      stats.tasks[TASK_LOST]++;

      forward(update, UPID(), framework);
    }

    // Tell the allocator about the recovered resources.
    allocator->recoverResources(
        frameworkId,
        slaveId,
        offeredResources,
        None());

    return;
  }


  // We need to transform the offered resources by considering
  // resources transformations like persistent disk acquisition.
  Resources transformedOfferedResources = offeredResources;

  // Accumulated resources used by launched tasks.
  Resources usedResources;

  size_t index = 0;
  foreach (const Future<bool>& authorization, authorizations.get()) {
    const TaskInfo& task = tasks[index++];

    // NOTE: The task will not be in 'pendingTasks' if 'killTask()'
    // for the task was called before we are here. No need to launch
    // the task if it's no longer pending. However, we still need to
    // check the authorization result and do the validation so that we
    // can send status update in case the task has duplicated ID.
    bool pending = framework->pendingTasks.contains(task.task_id());

    // Remove from pending tasks.
    framework->pendingTasks.erase(task.task_id());

    // Check authorization result.
    CHECK(!authorization.isDiscarded());

    if (authorization.isFailed() || !authorization.get()) {
      string user = framework->info.user(); // Default user.
      if (task.has_command() && task.command().has_user()) {
        user = task.command().user();
      } else if (task.has_executor() && task.executor().command().has_user()) {
        user = task.executor().command().user();
      }

      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_ERROR,
          TaskStatus::SOURCE_MASTER,
          authorization.isFailed() ?
              "Authorization failure: " + authorization.failure() :
              "Not authorized to launch as user '" + user + "'",
          TaskStatus::REASON_TASK_UNAUTHORIZED);

      metrics.tasks_error++;
      stats.tasks[TASK_ERROR]++;

      forward(update, UPID(), framework);

      continue;
    }

    // Validate the task.
    const Option<Error>& validationError = validateTask(
        task,
        framework,
        slave,
        transformedOfferedResources,
        usedResources);

    if (validationError.isSome()) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_ERROR,
          TaskStatus::SOURCE_MASTER,
          validationError.get().message,
          TaskStatus::REASON_TASK_INVALID);

      metrics.tasks_error++;
      stats.tasks[TASK_ERROR]++;

      forward(update, UPID(), framework);

      continue;
    }

    // Add task.
    if (pending) {
      usedResources += addTask(task, framework, slave);

      // We allow frameworks to implicitly acquire persistent disk
      // through resources, meaning that they can transform the
      // offered resources. We need to infer those acquisitions.
      Owned<Resources::CompositeTransformation> transformation(
          new Resources::CompositeTransformation());

      foreach (const Resource& disk, usedResources.persistentDisks()) {
        if (!transformedOfferedResources.contains(disk)) {
          // NOTE: No need to check duplicated persistence ID because
          // it should have been validated in ResourceUsageValidator.
          transformation->add(Resources::AcquirePersistentDisk(disk));
        }
      }

      // Adjust the total resources by applying the transformation.
      Try<Resources> _transformedOfferedResources =
        (*transformation)(transformedOfferedResources);

      // NOTE: The transformation should have also been validated in
      // ResourceUsageValidator.
      CHECK_SOME(_transformedOfferedResources);
      transformedOfferedResources = _transformedOfferedResources.get();

      // TODO(jieyu): Ideally, we should just call 'share()' here.
      // However, Shared currently does not support implicit upcast
      // (i.e., we cannot implicitly convert from Shared<Derived> to
      // Shared<Base>). Revisit this once Shared starts to support
      // implicit upcast.
      allocator->transformAllocation(
          frameworkId,
          slaveId,
          Shared<Resources::Transformation>(transformation.release()));

      // TODO(bmahler): Consider updating this log message to indicate
      // when the executor is also being launched.
      LOG(INFO) << "Launching task " << task.task_id()
                << " of framework " << *framework
                << " with resources " << task.resources()
                << " on slave " << *slave;

      RunTaskMessage message;
      message.mutable_framework()->MergeFrom(framework->info);
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      message.mutable_task()->MergeFrom(task);

      send(slave->pid, message);
    }
  }

  // Calculate unused resources.
  Resources unusedResources = transformedOfferedResources - usedResources;

  if (!unusedResources.empty()) {
    // Tell the allocator about the unused (e.g., refused) resources.
    allocator->recoverResources(
        frameworkId, slaveId, unusedResources, filters);
  }
}


void Master::reviveOffers(const UPID& from, const FrameworkID& frameworkId)
{
  ++metrics.messages_revive_offers;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << frameworkId
      << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring revive offers message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  LOG(INFO) << "Reviving offers for framework " << *framework;
  allocator->reviveOffers(framework->id);
}


void Master::killTask(
    const UPID& from,
    const FrameworkID& frameworkId,
    const TaskID& taskId)
{
  ++metrics.messages_kill_task;

  LOG(INFO) << "Asked to kill task " << taskId
            << " of framework " << frameworkId;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId << " of framework "
      << frameworkId << " because the framework cannot be found";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring kill task message for task " << taskId << " of framework "
      << *framework << " because it is not expected from " << from;
    return;
  }

  if (framework->pendingTasks.contains(taskId)) {
    // Remove from pending tasks.
    framework->pendingTasks.erase(taskId);

    const StatusUpdate& update = protobuf::createStatusUpdate(
        frameworkId,
        None(),
        taskId,
        TASK_KILLED,
        TaskStatus::SOURCE_MASTER,
        "Killed pending task");

    forward(update, UPID(), framework);

    return;
  }

  Task* task = framework->getTask(taskId);
  if (task == NULL) {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << *framework
                 << " because it is unknown; performing reconciliation";

    TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskId);

    _reconcileTasks(framework, {status});
    return;
  }

  Slave* slave = getSlave(task->slave_id());
  CHECK(slave != NULL) << "Unknown slave " << task->slave_id();

  // We add the task to 'killedTasks' here because the slave
  // might be partitioned or disconnected but the master
  // doesn't know it yet.
  slave->killedTasks.put(frameworkId, taskId);

  // NOTE: This task will be properly reconciled when the
  // disconnected slave re-registers with the master.
  if (slave->connected) {
    LOG(INFO) << "Telling slave " << *slave
              << " to kill task " << taskId
              << " of framework " << *framework;

    KillTaskMessage message;
    message.mutable_framework_id()->MergeFrom(frameworkId);
    message.mutable_task_id()->MergeFrom(taskId);
    send(slave->pid, message);
  } else {
    LOG(WARNING) << "Cannot kill task " << taskId
                 << " of framework " << *framework
                 << " because the slave " << *slave << " is disconnected."
                 << " Kill will be retried if the slave re-registers";
  }
}


void Master::statusUpdateAcknowledgement(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const TaskID& taskId,
    const string& uuid)
{
  metrics.messages_status_update_acknowledgement++;

  // TODO(bmahler): Consider adding a message validator abstraction
  // for the master that takes care of all this boilerplate. Ideally
  // by the time we process messages in the critical master code, we
  // can assume that they are valid. This will become especially
  // important as validation logic is moved out of the scheduler
  // driver and into the master.

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement message for task " << taskId
      << " of framework " << frameworkId << " on slave " << slaveId
      << " because the framework cannot be found";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring status update acknowledgement message for task " << taskId
      << " of framework " << *framework << " on slave " << slaveId
      << " because it is not expected from " << from;
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  Slave* slave = getSlave(slaveId);

  if (slave == NULL) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement message for task " << taskId
      << " of framework " << *framework << " to slave " << slaveId
      << " because slave is not registered";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  if (!slave->connected) {
    LOG(WARNING)
      << "Cannot send status update acknowledgement message for task " << taskId
      << " of framework " << *framework << " to slave " << *slave
      << " because slave is disconnected";
    metrics.invalid_status_update_acknowledgements++;
    return;
  }

  Task* task = slave->getTask(frameworkId, taskId);

  if (task != NULL) {
    // Status update state and uuid should be either set or unset
    // together.
    CHECK_EQ(task->has_status_update_uuid(), task->has_status_update_state());

    if (!task->has_status_update_state()) {
      // Task should have status update state set because it must have
      // been set when the update corresponding to this
      // acknowledgement was processed by the master. But in case this
      // acknowledgement was intended for the old run of the master
      // and the task belongs to a 0.20.0 slave, we could be here.
      // Dropping the acknowledgement is safe because the slave will
      // retry the update, at which point the master will set the
      // status update state.
      LOG(ERROR)
        << "Ignoring status update acknowledgement message for task " << taskId
        << " of framework " << *framework << " to slave " << *slave
        << " because it no update was sent by this master";
      metrics.invalid_status_update_acknowledgements++;
      return;
    }

    // Remove the task once the terminal update is acknowledged.
    if (protobuf::isTerminalState(task->status_update_state()) &&
        task->status_update_uuid() == uuid) {
      removeTask(task);
     }
  }

  LOG(INFO) << "Forwarding status update acknowledgement "
            << UUID::fromBytes(uuid) << " for task " << taskId
            << " of framework " << *framework << " to slave " << *slave;

  StatusUpdateAcknowledgementMessage message;
  message.mutable_slave_id()->CopyFrom(slaveId);
  message.mutable_framework_id()->CopyFrom(frameworkId);
  message.mutable_task_id()->CopyFrom(taskId);
  message.set_uuid(uuid);

  send(slave->pid, message);

  metrics.valid_status_update_acknowledgements++;
}


void Master::schedulerMessage(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    const string& data)
{
  ++metrics.messages_framework_to_executor;

  Framework* framework = getFramework(frameworkId);

  if (framework == NULL) {
    LOG(WARNING)
      << "Ignoring framework message for executor " << executorId
      << " of framework " << frameworkId
      << " because the framework cannot be found";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring framework message for executor " << executorId
      << " of framework " << *framework
      << " because it is not expected from " << from;
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  Slave* slave = getSlave(slaveId);
  if (slave == NULL) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << *framework << " to slave " << slaveId
                 << " because slave is not registered";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  if (!slave->connected) {
    LOG(WARNING) << "Cannot send framework message for framework "
                 << *framework << " to slave " << *slave
                 << " because slave is disconnected";
    stats.invalidFrameworkMessages++;
    metrics.invalid_framework_to_executor_messages++;
    return;
  }

  LOG(INFO) << "Sending framework message for framework "
            << *framework << " to slave " << *slave;

  FrameworkToExecutorMessage message;
  message.mutable_slave_id()->MergeFrom(slaveId);
  message.mutable_framework_id()->MergeFrom(frameworkId);
  message.mutable_executor_id()->MergeFrom(executorId);
  message.set_data(data);
  send(slave->pid, message);

  stats.validFrameworkMessages++;
  metrics.valid_framework_to_executor_messages++;
}


void Master::registerSlave(
    const UPID& from,
    const SlaveInfo& slaveInfo,
    const string& version)
{
  ++metrics.messages_register_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(), &Self::registerSlave, from, slaveInfo, version));
    return;
  }

  if (flags.authenticate_slaves && !authenticated.contains(from)) {
    // This could happen if another authentication request came
    // through before we are here or if a slave tried to register
    // without authentication.
    LOG(WARNING) << "Refusing registration of slave at " << from
                 << " because it is not authenticated";
    ShutdownMessage message;
    message.set_message("Slave is not authenticated");
    send(from, message);
    return;
  }

  // Check if this slave is already registered (because it retries).
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->pid == from) {
      if (!slave->connected) {
        // The slave was previously disconnected but it is now trying
        // to register as a new slave. This could happen if the slave
        // failed recovery and hence registering as a new slave before
        // the master removed the old slave from its map.
        LOG(INFO)
          << "Removing old disconnected slave " << *slave
          << " because a registration attempt is being made from " << from;
        removeSlave(slave);
        break;
      } else {
        CHECK(slave->active)
            << "Unexpected connected but deactivated slave " << *slave;

        LOG(INFO) << "Slave " << *slave << " already registered,"
                  << " resending acknowledgement";
        SlaveRegisteredMessage message;
        message.mutable_slave_id()->MergeFrom(slave->id);
        send(from, message);
        return;
      }
    }
  }

  // We need to generate a SlaveID and admit this slave only *once*.
  if (slaves.registering.contains(from)) {
    LOG(INFO) << "Ignoring register slave message from " << from
              << " (" << slaveInfo.hostname() << ") as admission is"
              << " already in progress";
    return;
  }

  slaves.registering.insert(from);

  // Create and add the slave id.
  SlaveInfo slaveInfo_ = slaveInfo;
  slaveInfo_.mutable_id()->CopyFrom(newSlaveId());

  LOG(INFO) << "Registering slave at " << from << " ("
            << slaveInfo.hostname() << ") with id " << slaveInfo_.id();

  registrar->apply(Owned<Operation>(new AdmitSlave(slaveInfo_)))
    .onAny(defer(self(),
                 &Self::_registerSlave,
                 slaveInfo_,
                 from,
                 version,
                 lambda::_1));
}


void Master::_registerSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const string& version,
    const Future<bool>& admit)
{
  slaves.registering.erase(pid);

  CHECK(!admit.isDiscarded());

  if (admit.isFailed()) {
    LOG(FATAL) << "Failed to admit slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << "): " << admit.failure();
  } else if (!admit.get()) {
    // This means the slave is already present in the registrar, it's
    // likely we generated a duplicate slave id!
    LOG(ERROR) << "Slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << ") was not admitted, "
               << "asking to shut down";
    slaves.removed.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to register but got duplicate slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    Slave* slave = new Slave(
        slaveInfo,
        pid,
        version.empty() ? Option<string>::none() : version,
        Clock::now());

    ++metrics.slave_registrations;

    addSlave(slave);

    SlaveRegisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);

    LOG(INFO) << "Registered slave " << *slave
              << " with " << slave->info.resources();
  }
}


void Master::reregisterSlave(
    const UPID& from,
    const SlaveInfo& slaveInfo,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks,
    const string& version)
{
  ++metrics.messages_reregister_slave;

  if (authenticating.contains(from)) {
    LOG(INFO) << "Queuing up re-registration request from " << from
              << " because authentication is still in progress";

    authenticating[from]
      .onReady(defer(self(),
                     &Self::reregisterSlave,
                     from,
                     slaveInfo,
                     executorInfos,
                     tasks,
                     completedFrameworks,
                     version));
    return;
  }

  if (flags.authenticate_slaves && !authenticated.contains(from)) {
    // This could happen if another authentication request came
    // through before we are here or if a slave tried to
    // re-register without authentication.
    LOG(WARNING) << "Refusing re-registration of slave at " << from
                 << " because it is not authenticated";
    ShutdownMessage message;
    message.set_message("Slave is not authenticated");
    send(from, message);
    return;
  }

  if (slaves.removed.get(slaveInfo.id()).isSome()) {
    // To compensate for the case where a non-strict registrar is
    // being used, we explicitly deny removed slaves from
    // re-registering. This is because a non-strict registrar cannot
    // enforce this. We've already told frameworks the tasks were
    // lost so it's important to deny the slave from re-registering.
    LOG(WARNING) << "Slave " << slaveInfo.id() << " at " << from
                 << " (" << slaveInfo.hostname() << ") attempted to "
                 << "re-register after removal; shutting it down";

    ShutdownMessage message;
    message.set_message("Slave attempted to re-register after removal");
    send(from, message);
    return;
  }

  Slave* slave = getSlave(slaveInfo.id());

  if (slave != NULL) {
    slave->reregisteredTime = Clock::now();

    // NOTE: This handles the case where a slave tries to
    // re-register with an existing master (e.g. because of a
    // spurious Zookeeper session expiration or after the slave
    // recovers after a restart).
    // For now, we assume this slave is not nefarious (eventually
    // this will be handled by orthogonal security measures like key
    // based authentication).
    LOG(INFO) << "Re-registering slave " << *slave;

    // Update the slave pid and relink to it.
    // NOTE: Re-linking the slave here always rather than only when
    // the slave is disconnected can lead to multiple exited events
    // in succession for a disconnected slave. As a result, we
    // ignore duplicate exited events for disconnected checkpointing
    // slaves.
    // See: https://issues.apache.org/jira/browse/MESOS-675
    slave->pid = from;
    link(slave->pid);

    // Reconcile tasks between master and the slave.
    // NOTE: This sends the re-registered message, including tasks
    // that need to be reconciled by the slave.
    reconcile(slave, executorInfos, tasks);

    // If this is a disconnected slave, add it back to the allocator.
    // This is done after reconciliation to ensure the allocator's
    // offers include the recovered resources initially on this
    // slave.
    if (!slave->connected) {
      slave->connected = true;
      dispatch(slave->observer, &SlaveObserver::reconnect);
      slave->active = true;
      allocator->activateSlave(slave->id);
    }

    CHECK(slave->active)
      << "Unexpected connected but deactivated slave " << *slave;

    // Inform the slave of the new framework pids for its tasks.
    __reregisterSlave(slave, tasks);

    return;
  }

  // Ensure we don't remove the slave for not re-registering after
  // we've recovered it from the registry.
  slaves.recovered.erase(slaveInfo.id());

  // If we're already re-registering this slave, then no need to ask
  // the registrar again.
  if (slaves.reregistering.contains(slaveInfo.id())) {
    LOG(INFO)
      << "Ignoring re-register slave message from slave "
      << slaveInfo.id() << " at " << from << " ("
      << slaveInfo.hostname() << ") as readmission is already in progress";
    return;
  }

  LOG(INFO) << "Re-registering slave " << slaveInfo.id() << " at " << from
            << " (" << slaveInfo.hostname() << ")";

  slaves.reregistering.insert(slaveInfo.id());

  // This handles the case when the slave tries to re-register with
  // a failed over master, in which case we must consult the
  // registrar.
  registrar->apply(Owned<Operation>(new ReadmitSlave(slaveInfo)))
    .onAny(defer(self(),
                 &Self::_reregisterSlave,
                 slaveInfo,
                 from,
                 executorInfos,
                 tasks,
                 completedFrameworks,
                 version,
                 lambda::_1));
}


void Master::_reregisterSlave(
    const SlaveInfo& slaveInfo,
    const UPID& pid,
    const vector<ExecutorInfo>& executorInfos,
    const vector<Task>& tasks,
    const vector<Archive::Framework>& completedFrameworks,
    const string& version,
    const Future<bool>& readmit)
{
  slaves.reregistering.erase(slaveInfo.id());

  CHECK(!readmit.isDiscarded());

  if (readmit.isFailed()) {
    LOG(FATAL) << "Failed to readmit slave " << slaveInfo.id() << " at " << pid
               << " (" << slaveInfo.hostname() << "): " << readmit.failure();
  } else if (!readmit.get()) {
    LOG(WARNING) << "The slave " << slaveInfo.id() << " at "
                 << pid << " (" << slaveInfo.hostname() << ") could not be"
                 << " readmitted; shutting it down";
    slaves.removed.put(slaveInfo.id(), Nothing());

    ShutdownMessage message;
    message.set_message(
        "Slave attempted to re-register with unknown slave id " +
        stringify(slaveInfo.id()));
    send(pid, message);
  } else {
    // Re-admission succeeded.
    Slave* slave = new Slave(
        slaveInfo,
        pid,
        version.empty() ? Option<string>::none() : version,
        Clock::now(),
        executorInfos,
        tasks);

    slave->reregisteredTime = Clock::now();

    ++metrics.slave_reregistrations;

    addSlave(slave, completedFrameworks);

    SlaveReregisteredMessage message;
    message.mutable_slave_id()->MergeFrom(slave->id);
    send(slave->pid, message);

    LOG(INFO) << "Re-registered slave " << *slave
              << " with " << slave->info.resources();

    __reregisterSlave(slave, tasks);
  }
}


void Master::__reregisterSlave(Slave* slave, const vector<Task>& tasks)
{
  // Send the latest framework pids to the slave.
  hashset<UPID> pids;
  foreach (const Task& task, tasks) {
    Framework* framework = getFramework(task.framework_id());
    if (framework != NULL && !pids.contains(framework->pid)) {
      UpdateFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      message.set_pid(framework->pid);
      send(slave->pid, message);

      pids.insert(framework->pid);
    }
  }
}


void Master::unregisterSlave(const UPID& from, const SlaveID& slaveId)
{
  ++metrics.messages_unregister_slave;

  LOG(INFO) << "Asked to unregister slave " << slaveId;

  Slave* slave = getSlave(slaveId);

  if (slave != NULL) {
    if (slave->pid != from) {
      LOG(WARNING) << "Ignoring unregister slave message from " << from
                   << " because it is not the slave " << slave->pid;
      return;
    }
    removeSlave(slave);
  }
}


// TODO(vinod): Since 0.22.0, we can use 'from' instead of 'pid'
// because the status updates will be sent by the slave.
void Master::statusUpdate(const StatusUpdate& update, const UPID& pid)
{
  ++metrics.messages_status_update;

  if (slaves.removed.get(update.slave_id()).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring status update " << update
                 << " from removed slave " << pid
                 << " with id " << update.slave_id() << " ; asking slave "
                 << " to shutdown";

    ShutdownMessage message;
    message.set_message("Status update from unknown slave");
    send(pid, message);

    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  Slave* slave = getSlave(update.slave_id());

  if (slave == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from unknown slave " << pid
                 << " with id " << update.slave_id();
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  Framework* framework = getFramework(update.framework_id());

  if (framework == NULL) {
    LOG(WARNING) << "Ignoring status update " << update
                 << " from slave " << *slave
                 << " because the framework is unknown";
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  // Forward the update to the framework.
  forward(update, pid, framework);

  // Lookup the task and see if we need to update anything locally.
  Task* task = slave->getTask(update.framework_id(), update.status().task_id());
  if (task == NULL) {
    LOG(WARNING) << "Could not lookup task for status update " << update
                 << " from slave " << *slave;
    stats.invalidStatusUpdates++;
    metrics.invalid_status_updates++;
    return;
  }

  LOG(INFO) << "Status update " << update << " from slave " << *slave;

  updateTask(task, update);

  // If the task is terminal and no acknowledgement is needed,
  // then remove the task now.
  if (protobuf::isTerminalState(task->state()) && pid == UPID()) {
    removeTask(task);
  }

  stats.validStatusUpdates++;
  metrics.valid_status_updates++;
}


void Master::forward(
    const StatusUpdate& update,
    const UPID& acknowledgee,
    Framework* framework)
{
  CHECK_NOTNULL(framework);

  if (!acknowledgee) {
    LOG(INFO) << "Sending status update " << update
              << (update.status().has_message()
                  ? " '" + update.status().message() + "'"
                  : "");
  } else {
    LOG(INFO) << "Forwarding status update " << update;
  }

  StatusUpdateMessage message;
  message.mutable_update()->MergeFrom(update);
  message.set_pid(acknowledgee);
  send(framework->pid, message);
}


void Master::exitedExecutor(
    const UPID& from,
    const SlaveID& slaveId,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId,
    int32_t status)
{
  ++metrics.messages_exited_executor;

  if (slaves.removed.get(slaveId).isSome()) {
    // If the slave is removed, we have already informed
    // frameworks that its tasks were LOST, so the slave should
    // shut down.
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on removed slave " << slaveId
                 << " ; asking slave to shutdown";

    ShutdownMessage message;
    message.set_message("Executor exited message from unknown slave");
    reply(message);
    return;
  }

  // Only update master's internal data structures here for proper
  // accounting. The TASK_LOST updates are handled by the slave.
  if (!slaves.registered.contains(slaveId)) {
    LOG(WARNING) << "Ignoring exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on unknown slave " << slaveId;
    return;
  }

  Slave* slave = CHECK_NOTNULL(slaves.registered[slaveId]);

  if (!slave->hasExecutor(frameworkId, executorId)) {
    LOG(WARNING) << "Ignoring unknown exited executor '" << executorId
                 << "' of framework " << frameworkId
                 << " on slave " << *slave;
    return;
  }

  LOG(INFO) << "Executor " << executorId
            << " of framework " << frameworkId
            << " on slave " << *slave << " "
            << WSTRINGIFY(status);

  removeExecutor(slave, frameworkId, executorId);

  // TODO(benh): Send the framework its executor's exit status?
  // Or maybe at least have something like Scheduler::executorLost?
}


void Master::shutdownSlave(const SlaveID& slaveId, const string& message)
{
  if (!slaves.registered.contains(slaveId)) {
    // Possible when the SlaveObserver dispatched to shutdown a slave,
    // but exited() was already called for this slave.
    LOG(WARNING) << "Unable to shutdown unknown slave " << slaveId;
    return;
  }

  Slave* slave = slaves.registered[slaveId];
  CHECK_NOTNULL(slave);

  LOG(WARNING) << "Shutting down slave " << *slave << " with message '"
               << message << "'";

  ShutdownMessage message_;
  message_.set_message(message);
  send(slave->pid, message_);

  removeSlave(slave);
}


void Master::reconcileTasks(
    const UPID& from,
    const FrameworkID& frameworkId,
    const std::vector<TaskStatus>& statuses)
{
  ++metrics.messages_reconcile_tasks;

  Framework* framework = getFramework(frameworkId);
  if (framework == NULL) {
    LOG(WARNING) << "Unknown framework " << frameworkId << " at " << from
                 << " attempted to reconcile tasks";
    return;
  }

  if (from != framework->pid) {
    LOG(WARNING)
      << "Ignoring reconcile tasks message for framework " << *framework
      << " because it is not expected from " << from;
    return;
  }

  _reconcileTasks(framework, statuses);
}


void Master::_reconcileTasks(
    Framework* framework,
    const vector<TaskStatus>& statuses)
{
  CHECK_NOTNULL(framework);

  if (statuses.empty()) {
    // Implicit reconciliation.
    LOG(INFO) << "Performing implicit task state reconciliation"
                 " for framework " << *framework;

    foreachvalue (const TaskInfo& task, framework->pendingTasks) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task.slave_id(),
          task.task_id(),
          TASK_STAGING,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION);

      VLOG(1) << "Sending implicit reconciliation state "
              << update.status().state()
              << " for task " << update.status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      send(framework->pid, message);
    }

    foreachvalue (Task* task, framework->tasks) {
      const TaskState& state = task->has_status_update_state()
          ? task->status_update_state()
          : task->state();

      const Option<ExecutorID>& executorId = task->has_executor_id()
          ? Option<ExecutorID>(task->executor_id())
          : None();

      const StatusUpdate& update = protobuf::createStatusUpdate(
          framework->id,
          task->slave_id(),
          task->task_id(),
          state,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION,
          executorId,
          protobuf::getTaskHealth(*task));

      VLOG(1) << "Sending implicit reconciliation state "
              << update.status().state()
              << " for task " << update.status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update);
      send(framework->pid, message);
    }

    return;
  }

  // Explicit reconciliation.
  LOG(INFO) << "Performing explicit task state reconciliation for "
            << statuses.size() << " tasks of framework " << *framework;

  // Explicit reconciliation occurs for the following cases:
  //   (1) Task is known, but pending: TASK_STAGING.
  //   (2) Task is known: send the latest state.
  //   (3) Task is unknown, slave is registered: TASK_LOST.
  //   (4) Task is unknown, slave is transitioning: no-op.
  //   (5) Task is unknown, slave is unknown: TASK_LOST.
  //
  // When using a non-strict registry, case (5) may result in
  // a TASK_LOST for a task that may later be non-terminal. This
  // is better than no reply at all because the framework can take
  // action for TASK_LOST. Later, if the task is running, the
  // framework can discover it with implicit reconciliation and will
  // be able to kill it.
  foreach (const TaskStatus& status, statuses) {
    Option<SlaveID> slaveId = None();
    if (status.has_slave_id()) {
      slaveId = status.slave_id();
    }

    Option<StatusUpdate> update = None();
    Task* task = framework->getTask(status.task_id());

    if (framework->pendingTasks.contains(status.task_id())) {
      // (1) Task is known, but pending: TASK_STAGING.
      const TaskInfo& task_ = framework->pendingTasks[status.task_id()];
      update = protobuf::createStatusUpdate(
          framework->id,
          task_.slave_id(),
          task_.task_id(),
          TASK_STAGING,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION);
    } else if (task != NULL) {
      // (2) Task is known: send the latest status update state.
      const TaskState& state = task->has_status_update_state()
          ? task->status_update_state()
          : task->state();

      const Option<ExecutorID> executorId = task->has_executor_id()
          ? Option<ExecutorID>(task->executor_id())
          : None();

      update = protobuf::createStatusUpdate(
          framework->id,
          task->slave_id(),
          task->task_id(),
          state,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Latest task state",
          TaskStatus::REASON_RECONCILIATION,
          executorId,
          protobuf::getTaskHealth(*task));
    } else if (slaveId.isSome() && slaves.registered.contains(slaveId.get())) {
      // (3) Task is unknown, slave is registered: TASK_LOST.
      update = protobuf::createStatusUpdate(
          framework->id,
          slaveId.get(),
          status.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Task is unknown to the slave",
          TaskStatus::REASON_RECONCILIATION);
    } else if (slaves.transitioning(slaveId)) {
      // (4) Task is unknown, slave is transitionary: no-op.
      LOG(INFO) << "Dropping reconciliation of task " << status.task_id()
                << " for framework " << *framework
                << " because there are transitional slaves";
    } else {
      // (5) Task is unknown, slave is unknown: TASK_LOST.
      update = protobuf::createStatusUpdate(
          framework->id,
          slaveId,
          status.task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          "Reconciliation: Task is unknown",
          TaskStatus::REASON_RECONCILIATION);
    }

    if (update.isSome()) {
      VLOG(1) << "Sending explicit reconciliation state "
              << update.get().status().state()
              << " for task " << update.get().status().task_id()
              << " of framework " << *framework;

      // TODO(bmahler): Consider using forward(); might lead to too
      // much logging.
      StatusUpdateMessage message;
      message.mutable_update()->CopyFrom(update.get());
      send(framework->pid, message);
    }
  }
}


void Master::frameworkFailoverTimeout(const FrameworkID& frameworkId,
                                      const Time& reregisteredTime)
{
  Framework* framework = getFramework(frameworkId);

  if (framework != NULL && !framework->connected) {
    // If the re-registration time has not changed, then the framework
    // has not re-registered within the failover timeout.
    if (framework->reregisteredTime == reregisteredTime) {
      LOG(INFO) << "Framework failover timeout, removing framework "
                << *framework;
      removeFramework(framework);
    }
  }
}


void Master::offer(const FrameworkID& frameworkId,
                   const hashmap<SlaveID, Resources>& resources)
{
  if (!frameworks.registered.contains(frameworkId) ||
      !frameworks.registered[frameworkId]->active) {
    LOG(WARNING) << "Master returning resources offered to framework "
                 << frameworkId << " because the framework"
                 << " has terminated or is inactive";

    foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
      allocator->recoverResources(frameworkId, slaveId, offered, None());
    }
    return;
  }

  // Create an offer for each slave and add it to the message.
  ResourceOffersMessage message;

  Framework* framework = CHECK_NOTNULL(frameworks.registered[frameworkId]);
  foreachpair (const SlaveID& slaveId, const Resources& offered, resources) {
    if (!slaves.registered.contains(slaveId)) {
      LOG(WARNING)
        << "Master returning resources offered to framework " << *framework
        << " because slave " << slaveId << " is not valid";

      allocator->recoverResources(frameworkId, slaveId, offered, None());
      continue;
    }

    Slave* slave = slaves.registered[slaveId];

    CHECK(slave->info.checkpoint() || !framework->info.checkpoint())
        << "Resources of non checkpointing slave " << *slave
        << " are being offered to checkpointing framework " << *framework;

    // This could happen if the allocator dispatched 'Master::offer' before
    // the slave was deactivated in the allocator.
    if (!slave->active) {
      LOG(WARNING)
        << "Master returning resources offered because slave " << *slave
        << " is " << (slave->connected ? "deactivated" : "disconnected");

      allocator->recoverResources(frameworkId, slaveId, offered, None());
      continue;
    }

#ifdef WITH_NETWORK_ISOLATOR
    // TODO(dhamon): This flag is required as the static allocation of
    // ephemeral ports leads to a maximum number of containers that can
    // be created on each slave. Once MESOS-1654 is fixed and ephemeral
    // ports are a first class resource, this can be removed.
    if (flags.max_executors_per_slave.isSome()) {
      // Check that we haven't hit the executor limit.
      size_t numExecutors = 0;
      foreachkey (const FrameworkID& frameworkId, slave->executors) {
        numExecutors += slave->executors[frameworkId].keys().size();
      }

      if (numExecutors >= flags.max_executors_per_slave.get()) {
        LOG(WARNING) << "Master returning resources offered because slave "
                     << *slave << " has reached the maximum number of "
                     << "executors";
        // Pass a default filter to avoid getting this same offer immediately
        // from the allocator.
        allocator->recoverResources(frameworkId, slaveId, offered, Filters());
        continue;
      }
    }
#endif // WITH_NETWORK_ISOLATOR

    Offer* offer = new Offer();
    offer->mutable_id()->MergeFrom(newOfferId());
    offer->mutable_framework_id()->MergeFrom(framework->id);
    offer->mutable_slave_id()->MergeFrom(slave->id);
    offer->set_hostname(slave->info.hostname());
    offer->mutable_resources()->MergeFrom(offered);
    offer->mutable_attributes()->MergeFrom(slave->info.attributes());

    // Add all framework's executors running on this slave.
    if (slave->executors.contains(framework->id)) {
      const hashmap<ExecutorID, ExecutorInfo>& executors =
        slave->executors[framework->id];
      foreachkey (const ExecutorID& executorId, executors) {
        offer->add_executor_ids()->MergeFrom(executorId);
      }
    }

    offers[offer->id()] = offer;

    framework->addOffer(offer);
    slave->addOffer(offer);

    if (flags.offer_timeout.isSome()) {
      // Rescind the offer after the timeout elapses.
      offerTimers[offer->id()] =
        delay(flags.offer_timeout.get(),
              self(),
              &Self::offerTimeout,
              offer->id());
    }

    // TODO(jieyu): For now, we strip 'ephemeral_ports' resource from
    // offers so that frameworks do not see this resource. This is a
    // short term workaround. Revisit this once we resolve MESOS-1654.
    Offer offer_ = *offer;
    offer_.clear_resources();

    foreach (const Resource& resource, offered) {
      if (resource.name() != "ephemeral_ports") {
        offer_.add_resources()->CopyFrom(resource);
      }
    }

    // Add the offer *AND* the corresponding slave's PID.
    message.add_offers()->MergeFrom(offer_);
    message.add_pids(slave->pid);
  }

  if (message.offers().size() == 0) {
    return;
  }

  LOG(INFO) << "Sending " << message.offers().size()
            << " offers to framework " << *framework;

  send(framework->pid, message);
}


// TODO(vinod): If due to network partition there are two instances
// of the framework that think they are leaders and try to
// authenticate with master they would be stepping on each other's
// toes. Currently it is tricky to detect this case because the
// 'authenticate' message doesn't contain the 'FrameworkID'.
void Master::authenticate(const UPID& from, const UPID& pid)
{
  ++metrics.messages_authenticate;

  // An authentication request is sent by a client (slave/framework)
  // in the following cases:
  //
  // 1. First time the client is connecting.
  //    This is straightforward; just proceed with authentication.
  //
  // 2. Client retried because of ZK expiration / authentication timeout.
  //    If the client is already authenticated, it will be removed from
  //    the 'authenticated' map and authentication is retried.
  //
  // 3. Client restarted.
  //   3.1. We are here after receiving 'exited()' from old client.
  //        This is safe because the client will be first marked as
  //        disconnected and then when it re-registers it will be
  //        marked as connected.
  //
  //  3.2. We are here before receiving 'exited()' from old client.
  //       This is tricky only if the PID of the client doesn't change
  //       after restart; true for slave but not for framework.
  //       If the PID doesn't change the master might mark the client
  //       disconnected *after* the client re-registers.
  //       This is safe because the client (slave) will be informed
  //       about this discrepancy via ping messages so that it can
  //       re-register.

  authenticated.erase(pid);

  if (authenticating.contains(pid)) {
    LOG(INFO) << "Queuing up authentication request from " << pid
              << " because authentication is still in progress";

    // Try to cancel the in progress authentication by deleting
    // the authenticator.
    authenticators.erase(pid);

    // Retry after the current authenticator finishes.
    authenticating[pid]
      .onAny(defer(self(), &Self::authenticate, from, pid));

    return;
  }

  LOG(INFO) << "Authenticating " << pid;

  // Create a promise to capture the entire "authenticating"
  // procedure. We'll set this _after_ we finish _authenticate.
  Owned<Promise<Nothing>> promise(new Promise<Nothing>());

  // Create and initialize the authenticator.
  Authenticator* authenticator;
  // TODO(tillt): Allow multiple authenticators to be loaded and enable
  // the authenticatee to select the appropriate one. See MESOS-1939.
  if (authenticatorNames[0] == DEFAULT_AUTHENTICATOR) {
    LOG(INFO) << "Using default CRAM-MD5 authenticator";
    authenticator = new cram_md5::CRAMMD5Authenticator();
  } else {
    Try<Authenticator*> module =
      modules::ModuleManager::create<Authenticator>(authenticatorNames[0]);
    if (module.isError()) {
      EXIT(1) << "Could not create authenticator module '"
              << authenticatorNames[0] << "': " << module.error();
    }
    LOG(INFO) << "Using '" << authenticatorNames[0] << "' authenticator";
    authenticator = module.get();
  }
  Owned<Authenticator> authenticator_ = Owned<Authenticator>(authenticator);

  authenticator_->initialize(from);

  // Start authentication.
  const Future<Option<string>>& future = authenticator_->authenticate()
     .onAny(defer(self(), &Self::_authenticate, pid, promise, lambda::_1));

  // Don't wait for authentication to happen for ever.
  delay(Seconds(5),
        self(),
        &Self::authenticationTimeout,
        future);

  // Save our state.
  authenticating[pid] = promise->future();
  authenticators.put(pid, authenticator_);
}


void Master::_authenticate(
    const UPID& pid,
    const Owned<Promise<Nothing>>& promise,
    const Future<Option<string>>& future)
{
  if (!future.isReady() || future.get().isNone()) {
    const string& error = future.isReady()
        ? "Refused authentication"
        : (future.isFailed() ? future.failure() : "future discarded");

    LOG(WARNING) << "Failed to authenticate " << pid
                 << ": " << error;

    promise->fail(error);
  } else {
    LOG(INFO) << "Successfully authenticated principal '" << future.get().get()
              << "' at " << pid;

    promise->set(Nothing());
    authenticated.put(pid, future.get().get());
  }

  authenticators.erase(pid);
  authenticating.erase(pid);
}


void Master::authenticationTimeout(Future<Option<string>> future)
{
  // Note that a 'discard' here is safe even if another
  // authenticator is in progress because this copy of the future
  // corresponds to the original authenticator that started the timer.
  if (future.discard()) { // This is a no-op if the future is already ready.
    LOG(WARNING) << "Authentication timed out";
  }
}


// NOTE: This function is only called when the slave re-registers
// with a master that already knows about it (i.e., not a failed
// over master).
void Master::reconcile(
    Slave* slave,
    const vector<ExecutorInfo>& executors,
    const vector<Task>& tasks)
{
  CHECK_NOTNULL(slave);

  // TODO(bmahler): There's an implicit assumption here the slave
  // cannot have tasks unknown to the master. This _should_ be the
  // case since the causal relationship is:
  //   slave removes task -> master removes task
  // Add error logging for any violations of this assumption!

  // We convert the 'tasks' into a map for easier lookup below.
  multihashmap<FrameworkID, TaskID> slaveTasks;
  foreach (const Task& task, tasks) {
    slaveTasks.put(task.framework_id(), task.task_id());
  }

  // Look for tasks missing in the slave's re-registration message.
  // This can occur when:
  //   (1) a launch message was dropped (e.g. slave failed over), or
  //   (2) the slave re-registration raced with a launch message, in
  //       which case the slave actually received the task.
  // To resolve both cases correctly, we must reconcile through the
  // slave. For slaves that do not support reconciliation, we keep
  // the old semantics and cover only case (1) via TASK_LOST.
  SlaveReregisteredMessage reregistered;
  reregistered.mutable_slave_id()->MergeFrom(slave->id);

  // NOTE: copies are needed because removeTask modified slave->tasks.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
    ReconcileTasksMessage reconcile;
    reconcile.mutable_framework_id()->CopyFrom(frameworkId);

    foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
      if (!slaveTasks.contains(task->framework_id(), task->task_id())) {
        LOG(WARNING) << "Task " << task->task_id()
                     << " of framework " << task->framework_id()
                     << " unknown to the slave " << *slave
                     << " during re-registration"
                     << (slave->version.isSome()
                         ? ": reconciling with the slave"
                         : ": sending TASK_LOST");

        if (slave->version.isSome()) {
          // NOTE: Currently the slave doesn't look at the task state
          // when it reconciles the task state; we include the correct
          // state for correctness and consistency.
          const TaskState& state = task->has_status_update_state()
              ? task->status_update_state()
              : task->state();

          TaskStatus* status = reconcile.add_statuses();
          status->mutable_task_id()->CopyFrom(task->task_id());
          status->mutable_slave_id()->CopyFrom(slave->id);
          status->set_state(state);
          status->set_source(TaskStatus::SOURCE_MASTER);
          status->set_message("Reconciliation request");
          status->set_reason(TaskStatus::REASON_RECONCILIATION);
          status->set_timestamp(Clock::now().secs());
        } else {
          // TODO(bmahler): Remove this case in 0.22.0.
          const StatusUpdate& update = protobuf::createStatusUpdate(
              task->framework_id(),
              slave->id,
              task->task_id(),
              TASK_LOST,
              TaskStatus::SOURCE_MASTER,
              "Task is unknown to the slave",
              TaskStatus::REASON_TASK_UNKNOWN);

          updateTask(task, update);
          removeTask(task);

          Framework* framework = getFramework(frameworkId);
          if (framework != NULL) {
            forward(update, UPID(), framework);
          }
        }
      }
    }

    if (slave->version.isSome() && reconcile.statuses_size() > 0) {
      reregistered.add_reconciliations()->CopyFrom(reconcile);
    }
  }

  // Re-register the slave.
  send(slave->pid, reregistered);

  // Likewise, any executors that are present in the master but
  // not present in the slave must be removed to correctly account
  // for resources. First we index the executors for fast lookup below.
  multihashmap<FrameworkID, ExecutorID> slaveExecutors;
  foreach (const ExecutorInfo& executor, executors) {
    // TODO(bmahler): The slave ensures the framework id is set in the
    // framework info when re-registering. This can be killed in 0.15.0
    // as we've added code in 0.14.0 to ensure the framework id is set
    // in the scheduler driver.
    if (!executor.has_framework_id()) {
      LOG(ERROR) << "Slave " << *slave
                 << " re-registered with executor " << executor.executor_id()
                 << " without setting the framework id";
      continue;
    }
    slaveExecutors.put(executor.framework_id(), executor.executor_id());
  }

  // Now that we have the index for lookup, remove all the executors
  // in the master that are not known to the slave.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[frameworkId])) {
      if (!slaveExecutors.contains(frameworkId, executorId)) {
        // TODO(bmahler): Reconcile executors correctly between the
        // master and the slave, see:
        // MESOS-1466, MESOS-1800, and MESOS-1720.
        LOG(WARNING) << "Executor " << executorId
                     << " of framework " << frameworkId
                     << " possibly unknown to the slave " << *slave;

        removeExecutor(slave, frameworkId, executorId);
      }
    }
  }

  // Send KillTaskMessages for tasks in 'killedTasks' that are
  // still alive on the slave. This could happen if the slave
  // did not receive KillTaskMessage because of a partition or
  // disconnection.
  foreach (const Task& task, tasks) {
    if (!protobuf::isTerminalState(task.state()) &&
        slave->killedTasks.contains(task.framework_id(), task.task_id())) {
      LOG(WARNING) << " Slave " << *slave
                   << " has non-terminal task " << task.task_id()
                   << " that is supposed to be killed. Killing it now!";

      KillTaskMessage message;
      message.mutable_framework_id()->MergeFrom(task.framework_id());
      message.mutable_task_id()->MergeFrom(task.task_id());
      send(slave->pid, message);
    }
  }

  // Send ShutdownFrameworkMessages for frameworks that are completed.
  // This could happen if the message wasn't received by the slave
  // (e.g., slave was down, partitioned).
  // NOTE: This is a short-term hack because this information is lost
  // when the master fails over. Also, we only store a limited number
  // of completed frameworks.
  // TODO(vinod): Revisit this when registrar is in place. It would
  // likely involve storing this information in the registrar.
  foreach (const shared_ptr<Framework>& framework, frameworks.completed) {
    if (slaveTasks.contains(framework->id)) {
      LOG(WARNING) << "Slave " << *slave
                   << " re-registered with completed framework " << *framework
                   << ". Shutting down the framework on the slave";

      ShutdownFrameworkMessage message;
      message.mutable_framework_id()->MergeFrom(framework->id);
      send(slave->pid, message);
    }
  }
}


void Master::addFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  CHECK(!frameworks.registered.contains(framework->id))
    << "Framework " << *framework << " already exists!";

  frameworks.registered[framework->id] = framework;

  link(framework->pid);

  // Enforced by Master::registerFramework.
  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << *framework;

  roles[framework->info.role()]->addFramework(framework);

  // There should be no offered resources yet!
  CHECK_EQ(Resources(), framework->offeredResources);

  allocator->addFramework(
      framework->id,
      framework->info,
      framework->usedResources);

  // Export framework metrics.

  // If the framework is authenticated, its principal should be in
  // 'authenticated'. Otherwise look if it's supplied in
  // FrameworkInfo.
  Option<string> principal = authenticated.get(framework->pid);
  if (principal.isNone() && framework->info.has_principal()) {
    principal = framework->info.principal();
  }

  CHECK(!frameworks.principals.contains(framework->pid));
  frameworks.principals.put(framework->pid, principal);

  // Export framework metrics if a principal is specified.
  if (principal.isSome()) {
    // Create new framework metrics if this framework is the first
    // one of this principal. Otherwise existing metrics are reused.
    if (!metrics.frameworks.contains(principal.get())) {
      metrics.frameworks.put(
          principal.get(),
          Owned<Metrics::Frameworks>(new Metrics::Frameworks(principal.get())));
    }
  }
}


// Replace the scheduler for a framework with a new process ID, in the
// event of a scheduler failover.
void Master::failoverFramework(Framework* framework, const UPID& newPid)
{
  const UPID oldPid = framework->pid;

  // There are a few failover cases to consider:
  //   1. The pid has changed. In this case we definitely want to
  //      send a FrameworkErrorMessage to shut down the older
  //      scheduler.
  //   2. The pid has not changed.
  //      2.1 The old scheduler on that pid failed over to a new
  //          instance on the same pid. No need to shut down the old
  //          scheduler as it is necessarily dead.
  //      2.2 This is a duplicate message. In this case, the scheduler
  //          has not failed over, so we do not want to shut it down.
  if (oldPid != newPid) {
    FrameworkErrorMessage message;
    message.set_message("Framework failed over");
    send(oldPid, message);
  }

  // TODO(benh): unlink(oldPid);

  framework->pid = newPid;
  link(newPid);

  // The scheduler driver safely ignores any duplicate registration
  // messages, so we don't need to compare the old and new pids here.
  FrameworkRegisteredMessage message;
  message.mutable_framework_id()->MergeFrom(framework->id);
  message.mutable_master_info()->MergeFrom(info_);
  send(newPid, message);

  // Remove the framework's offers (if they weren't removed before).
  // We do this after we have updated the pid and sent the framework
  // registered message so that the allocator can immediately re-offer
  // these resources to this framework if it wants.
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());
    removeOffer(offer);
  }

  framework->connected = true;

  // Reactivate the framework.
  // NOTE: We do this after recovering resources (above) so that
  // the allocator has the correct view of the framework's share.
  if (!framework->active) {
    framework->active = true;
    allocator->activateFramework(framework->id);
  }

  // 'Failover' the framework's metrics. i.e., change the lookup key
  // for its metrics to 'newPid'.
  if (oldPid != newPid && frameworks.principals.contains(oldPid)) {
    frameworks.principals[newPid] = frameworks.principals[oldPid];
    frameworks.principals.erase(oldPid);
  }
}


void Master::removeFramework(Framework* framework)
{
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << *framework;

  if (framework->active) {
    // Tell the allocator to stop allocating resources to this framework.
    // TODO(vinod): Consider setting  framework->active to false here
    // or just calling 'deactivate(Framework*)'.
    allocator->deactivateFramework(framework->id);
  }

  // Tell slaves to shutdown the framework.
  foreachvalue (Slave* slave, slaves.registered) {
    ShutdownFrameworkMessage message;
    message.mutable_framework_id()->MergeFrom(framework->id);
    send(slave->pid, message);
  }

  // Remove the pending tasks from the framework.
  framework->pendingTasks.clear();

  // Remove pointers to the framework's tasks in slaves.
  foreachvalue (Task* task, utils::copy(framework->tasks)) {
    Slave* slave = getSlave(task->slave_id());
    // Since we only find out about tasks when the slave re-registers,
    // it must be the case that the slave exists!
    CHECK(slave != NULL)
      << "Unknown slave " << task->slave_id()
      << " for task " << task->task_id();

    // The task is implicitly killed, and TASK_KILLED is the closest
    // state we have by now. We mark the task and remove it, without
    // sending the update. However, a task may finish during the
    // executor graceful shutdown period. By marking such task as
    // killed and moving it to completed, we lose the opportunity to
    // collect the possible finished status. We tolerate this,
    // because we expect that if the framework has been asked to shut
    // down, its user is not interested in results anymore.
    // TODO(alex): Consider a more descriptive state, e.g. TASK_ABANDONED.
    const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_KILLED,
        TaskStatus::SOURCE_MASTER,
        "Framework " + framework->id.value() + " removed",
        TaskStatus::REASON_FRAMEWORK_REMOVED,
        (task->has_executor_id()
         ? Option<ExecutorID>(task->executor_id())
         : None()));

    updateTask(task, update);
    removeTask(task);
  }

  // Remove the framework's offers (if they weren't removed before).
  foreach (Offer* offer, utils::copy(framework->offers)) {
    allocator->recoverResources(
        offer->framework_id(),
        offer->slave_id(),
        offer->resources(),
        None());
    removeOffer(offer);
  }

  // Remove the framework's executors for correct resource accounting.
  foreachkey (const SlaveID& slaveId, utils::copy(framework->executors)) {
    Slave* slave = getSlave(slaveId);
    if (slave != NULL) {
      foreachkey (const ExecutorID& executorId,
                  utils::copy(framework->executors[slaveId])) {
        removeExecutor(slave, framework->id, executorId);
      }
    }
  }

  // TODO(benh): Similar code between removeFramework and
  // failoverFramework needs to be shared!

  // TODO(benh): unlink(framework->pid);

  framework->unregisteredTime = Clock::now();

  // The completedFramework buffer now owns the framework pointer.
  frameworks.completed.push_back(shared_ptr<Framework>(framework));

  CHECK(roles.contains(framework->info.role()))
    << "Unknown role " << framework->info.role()
    << " of framework " << *framework;

  roles[framework->info.role()]->removeFramework(framework);

  // Remove the framework from authenticated.
  authenticated.erase(framework->pid);

  CHECK(frameworks.principals.contains(framework->pid));
  const Option<string> principal = frameworks.principals[framework->pid];

  frameworks.principals.erase(framework->pid);

  // Remove the framework's message counters.
  if (principal.isSome()) {
    // Remove the metrics for the principal if this framework is the
    // last one with this principal.
    if (!frameworks.principals.containsValue(principal.get())) {
      CHECK(metrics.frameworks.contains(principal.get()));
      metrics.frameworks.erase(principal.get());
    }
  }

  // Remove the framework.
  frameworks.registered.erase(framework->id);
  allocator->removeFramework(framework->id);
}


void Master::removeFramework(Slave* slave, Framework* framework)
{
  CHECK_NOTNULL(slave);
  CHECK_NOTNULL(framework);

  LOG(INFO) << "Removing framework " << *framework
            << " from slave " << *slave;

  // Remove pointers to framework's tasks in slaves, and send status
  // updates.
  // NOTE: A copy is needed because removeTask modifies slave->tasks.
  foreachvalue (Task* task, utils::copy(slave->tasks[framework->id])) {
    // Remove tasks that belong to this framework.
    if (task->framework_id() == framework->id) {
      // A framework might not actually exist because the master failed
      // over and the framework hasn't reconnected yet. For more info
      // please see the comments in 'removeFramework(Framework*)'.
      const StatusUpdate& update = protobuf::createStatusUpdate(
        task->framework_id(),
        task->slave_id(),
        task->task_id(),
        TASK_LOST,
        TaskStatus::SOURCE_MASTER,
        "Slave " + slave->info.hostname() + " disconnected",
        TaskStatus::REASON_SLAVE_DISCONNECTED,
        (task->has_executor_id()
            ? Option<ExecutorID>(task->executor_id()) : None()));

      updateTask(task, update);
      removeTask(task);
      forward(update, UPID(), framework);
    }
  }

  // Remove the framework's executors from the slave and framework
  // for proper resource accounting.
  if (slave->executors.contains(framework->id)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[framework->id])) {
      removeExecutor(slave, framework->id, executorId);
    }
  }
}


void Master::addSlave(
    Slave* slave,
    const vector<Archive::Framework>& completedFrameworks)
{
  CHECK_NOTNULL(slave);

  slaves.removed.erase(slave->id);
  slaves.registered[slave->id] = slave;

  link(slave->pid);

  // Set up an observer for the slave.
  slave->observer = new SlaveObserver(
      slave->pid, slave->info, slave->id, self());

  spawn(slave->observer);

  // Add the slave's executors to the frameworks.
  foreachkey (const FrameworkID& frameworkId, slave->executors) {
    foreachvalue (const ExecutorInfo& executorInfo,
                  slave->executors[frameworkId]) {
      Framework* framework = getFramework(frameworkId);
      if (framework != NULL) { // The framework might not be re-registered yet.
        framework->addExecutor(slave->id, executorInfo);
      }
    }
  }

  // Add the slave's tasks to the frameworks.
  foreachkey (const FrameworkID& frameworkId, slave->tasks) {
    foreachvalue (Task* task, slave->tasks[frameworkId]) {
      Framework* framework = getFramework(task->framework_id());
      if (framework != NULL) { // The framework might not be re-registered yet.
        framework->addTask(task);
      } else {
        // TODO(benh): We should really put a timeout on how long we
        // keep tasks running on a slave that never have frameworks
        // reregister and claim them.
        LOG(WARNING) << "Possibly orphaned task " << task->task_id()
                     << " of framework " << task->framework_id()
                     << " running on slave " << *slave;
      }
    }
  }

  // Re-add completed tasks reported by the slave.
  // Note that a slave considers a framework completed when it has no
  // tasks/executors running for that framework. But a master considers a
  // framework completed when the framework is removed after a failover timeout.
  // TODO(vinod): Reconcile the notion of a completed framework across the
  // master and slave.
  foreach (const Archive::Framework& completedFramework, completedFrameworks) {
    const FrameworkID& frameworkId = completedFramework.framework_info().id();
    Framework* framework = getFramework(frameworkId);
    foreach (const Task& task, completedFramework.tasks()) {
      if (framework != NULL) {
        VLOG(2) << "Re-adding completed task " << task.task_id()
                << " of framework " << *framework
                << " that ran on slave " << *slave;
        framework->addCompletedTask(task);
      } else {
        // We could be here if the framework hasn't registered yet.
        // TODO(vinod): Revisit these semantics when we store frameworks'
        // information in the registrar.
        LOG(WARNING) << "Possibly orphaned completed task " << task.task_id()
                     << " of framework " << task.framework_id()
                     << " that ran on slave " << *slave;
      }
    }
  }

  // TODO(bmahler): This will need to include resources that
  // are "persisted" on the slave (e.g. persistent volumes,
  // dynamic reservations, etc).
  allocator->addSlave(
      slave->id,
      slave->info,
      slave->info.resources(),
      slave->usedResources);
}


void Master::removeSlave(Slave* slave)
{
  CHECK_NOTNULL(slave);

  LOG(INFO) << "Removing slave " << *slave;

  // We want to remove the slave first, to avoid the allocator
  // re-allocating the recovered resources.
  //
  // NOTE: Removing the slave is not sufficient for recovering the
  // resources in the allocator, because the "Sorters" are updated
  // only within recoverResources() (see MESOS-621). The calls to
  // recoverResources() below are therefore required, even though
  // the slave is already removed.
  allocator->removeSlave(slave->id);

  // Transition the tasks to lost and remove them, BUT do not send
  // updates. Rather, build up the updates so that we can send them
  // after the slave is removed from the registry.
  vector<StatusUpdate> updates;
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->tasks)) {
    foreachvalue (Task* task, utils::copy(slave->tasks[frameworkId])) {
      const StatusUpdate& update = protobuf::createStatusUpdate(
          task->framework_id(),
          task->slave_id(),
          task->task_id(),
          TASK_LOST,
          TaskStatus::SOURCE_MASTER,
          "Slave " + slave->info.hostname() + " removed",
          TaskStatus::REASON_SLAVE_REMOVED,
          (task->has_executor_id() ?
              Option<ExecutorID>(task->executor_id()) : None()));

      updateTask(task, update);
      removeTask(task);

      updates.push_back(update);
    }
  }

  // Remove executors from the slave for proper resource accounting.
  foreachkey (const FrameworkID& frameworkId, utils::copy(slave->executors)) {
    foreachkey (const ExecutorID& executorId,
                utils::copy(slave->executors[frameworkId])) {
      removeExecutor(slave, frameworkId, executorId);
    }
  }

  foreach (Offer* offer, utils::copy(slave->offers)) {
    // TODO(vinod): We don't need to call 'Allocator::recoverResources'
    // once MESOS-621 is fixed.
    allocator->recoverResources(
        offer->framework_id(), slave->id, offer->resources(), None());

    // Remove and rescind offers.
    removeOffer(offer, true); // Rescind!
  }

  // Mark the slave as being removed.
  slaves.removing.insert(slave->id);
  slaves.registered.erase(slave->id);
  slaves.removed.put(slave->id, Nothing());
  authenticated.erase(slave->pid);

  // Kill the slave observer.
  terminate(slave->observer);
  wait(slave->observer);
  delete slave->observer;

  // TODO(benh): unlink(slave->pid);

  // Remove this slave from the registrar. Once this is completed, we
  // can forward the LOST task updates to the frameworks and notify
  // all frameworks that this slave was lost.
  registrar->apply(Owned<Operation>(new RemoveSlave(slave->info)))
    .onAny(defer(self(),
                 &Self::_removeSlave,
                 slave->info,
                 updates,
                 lambda::_1));

  delete slave;
}


void Master::_removeSlave(
    const SlaveInfo& slaveInfo,
    const vector<StatusUpdate>& updates,
    const Future<bool>& removed)
{
  slaves.removing.erase(slaveInfo.id());

  CHECK(!removed.isDiscarded());

  if (removed.isFailed()) {
    LOG(FATAL) << "Failed to remove slave " << slaveInfo.id()
               << " (" << slaveInfo.hostname() << ")"
               << " from the registrar: " << removed.failure();
  }

  CHECK(removed.get())
    << "Slave " << slaveInfo.id() << " (" << slaveInfo.hostname() << ") "
    << "already removed from the registrar";

  LOG(INFO) << "Removed slave " << slaveInfo.id() << " ("
            << slaveInfo.hostname() << ")";
  ++metrics.slave_removals;

  // Forward the LOST updates on to the framework.
  foreach (const StatusUpdate& update, updates) {
    Framework* framework = getFramework(update.framework_id());

    if (framework == NULL) {
      LOG(WARNING) << "Dropping update " << update << " from unknown framework "
                   << update.framework_id();
    } else {
      forward(update, UPID(), framework);
    }
  }

  // Notify all frameworks of the lost slave.
  foreachvalue (Framework* framework, frameworks.registered) {
    LOG(INFO) << "Notifying framework " << *framework << " of lost slave "
              << slaveInfo.id() << " (" << slaveInfo.hostname() << ") "
              << "after recovering";
    LostSlaveMessage message;
    message.mutable_slave_id()->MergeFrom(slaveInfo.id());
    send(framework->pid, message);
  }
}


void Master::updateTask(Task* task, const StatusUpdate& update)
{
  CHECK_NOTNULL(task);

  // Get the unacknowledged status.
  const TaskStatus& status = update.status();

  // Out-of-order updates should not occur, however in case they
  // do (e.g., due to bugs), prevent them here to ensure that the
  // resource accounting is not affected.
  if (protobuf::isTerminalState(task->state()) &&
      !protobuf::isTerminalState(status.state())) {
    LOG(ERROR) << "Ignoring out of order status update for task "
               << task->task_id()
               << " (" << task->state() << " -> " << status.state() << ")"
               << " of framework " << task->framework_id();
    return;
  }

  // Get the latest state.
  Option<TaskState> latestState;
  if (update.has_latest_state()) {
    latestState = update.latest_state();
  }

  // Set 'terminated' to true if this is the first time the task
  // transitioned to terminal state. Also set the latest state.
  bool terminated;
  if (latestState.isSome()) {
    // This update must be from >= 0.21.0 slave.
    terminated = !protobuf::isTerminalState(task->state()) &&
                 protobuf::isTerminalState(latestState.get());

    task->set_state(latestState.get());
  } else {
    // This update must be from a pre 0.21.0 slave or generated by the
    // master.
    terminated = !protobuf::isTerminalState(task->state()) &&
                 protobuf::isTerminalState(status.state());

    task->set_state(status.state());
  }

  // Set the status update state and uuid for the task.
  task->set_status_update_state(status.state());
  task->set_status_update_uuid(update.uuid());

  // TODO(brenden) Consider wiping the `message` field?
  if (task->statuses_size() > 0 &&
      task->statuses(task->statuses_size() - 1).state() == status.state()) {
    task->mutable_statuses()->RemoveLast();
  }
  task->add_statuses()->CopyFrom(status);

  // Delete data (maybe very large since it's stored by on-top framework) we
  // are not interested in to avoid OOM.
  // For example: mesos-master is running on a machine with 4GB free memory,
  // if every task stores 10MB data into TaskStatus, then mesos-master will be
  // killed by OOM killer after have 400 tasks finished.
  // MESOS-1746.
  task->mutable_statuses(task->statuses_size() - 1)->clear_data();

  LOG(INFO) << "Updating the latest state of task " << task->task_id()
            << " of framework " << task->framework_id()
            << " to " << task->state()
            << (task->state() != status.state()
                ? " (status update state: " + stringify(status.state()) + ")"
                : "");

  stats.tasks[status.state()]++;

  // Once the task becomes terminal, we recover the resources.
  if (terminated) {
    allocator->recoverResources(
        task->framework_id(),
        task->slave_id(),
        task->resources(),
        None());

    // The slave owns the Task object and cannot be NULL.
    Slave* slave = CHECK_NOTNULL(getSlave(task->slave_id()));
    slave->taskTerminated(task);

    Framework* framework = getFramework(task->framework_id());
    if (framework != NULL) {
      framework->taskTerminated(task);
    }

    switch (task->state()) {
      case TASK_FINISHED: ++metrics.tasks_finished; break;
      case TASK_FAILED:   ++metrics.tasks_failed;   break;
      case TASK_KILLED:   ++metrics.tasks_killed;   break;
      case TASK_LOST:     ++metrics.tasks_lost;     break;
      default: break;
    }
  }
}


void Master::removeTask(Task* task)
{
  CHECK_NOTNULL(task);

  // The slave owns the Task object and cannot be NULL.
  Slave* slave = CHECK_NOTNULL(getSlave(task->slave_id()));

  if (!protobuf::isTerminalState(task->state())) {
    LOG(WARNING) << "Removing task " << task->task_id()
                 << " with resources " << task->resources()
                 << " of framework " << task->framework_id()
                 << " on slave " << *slave
                 << " in non-terminal state " << task->state();

    // If the task is not terminal, then the resources have
    // not yet been recovered.
    allocator->recoverResources(
        task->framework_id(),
        task->slave_id(),
        task->resources(),
        None());
  } else {
    LOG(INFO) << "Removing task " << task->task_id()
              << " with resources " << task->resources()
              << " of framework " << task->framework_id()
              << " on slave " << *slave;
  }

  // Remove from framework.
  Framework* framework = getFramework(task->framework_id());
  if (framework != NULL) { // A framework might not be re-connected yet.
    framework->removeTask(task);
  }

  // Remove from slave.
  slave->removeTask(task);

  delete task;
}


void Master::removeExecutor(
    Slave* slave,
    const FrameworkID& frameworkId,
    const ExecutorID& executorId)
{
  CHECK_NOTNULL(slave);
  CHECK(slave->hasExecutor(frameworkId, executorId));

  ExecutorInfo executor = slave->executors[frameworkId][executorId];

  LOG(INFO) << "Removing executor '" << executorId
            << "' with resources " << executor.resources()
            << " of framework " << frameworkId << " on slave " << *slave;

  allocator->recoverResources(
    frameworkId, slave->id, executor.resources(), None());

  Framework* framework = getFramework(frameworkId);
  if (framework != NULL) { // The framework might not be re-registered yet.
    framework->removeExecutor(slave->id, executorId);
  }

  slave->removeExecutor(frameworkId, executorId);
}


void Master::offerTimeout(const OfferID& offerId)
{
  Offer* offer = getOffer(offerId);
  if (offer != NULL) {
    allocator->recoverResources(
        offer->framework_id(), offer->slave_id(), offer->resources(), None());
    removeOffer(offer, true);
  }
}


// TODO(vinod): Instead of 'removeOffer()', consider implementing
// 'useOffer()', 'discardOffer()' and 'rescindOffer()' for clarity.
void Master::removeOffer(Offer* offer, bool rescind)
{
  // Remove from framework.
  Framework* framework = getFramework(offer->framework_id());
  CHECK(framework != NULL)
    << "Unknown framework " << offer->framework_id()
    << " in the offer " << offer->id();

  framework->removeOffer(offer);

  // Remove from slave.
  Slave* slave = getSlave(offer->slave_id());
  CHECK(slave != NULL)
    << "Unknown slave " << offer->slave_id()
    << " in the offer " << offer->id();

  slave->removeOffer(offer);

  if (rescind) {
    RescindResourceOfferMessage message;
    message.mutable_offer_id()->MergeFrom(offer->id());
    send(framework->pid, message);
  }

  // Remove and cancel offer removal timers. Canceling the Timers is
  // only done to avoid having too many active Timers in libprocess.
  if (offerTimers.contains(offer->id())) {
    Clock::cancel(offerTimers[offer->id()]);
    offerTimers.erase(offer->id());
  }

  // Delete it.
  offers.erase(offer->id());
  delete offer;
}


// TODO(bmahler): Consider killing this.
Framework* Master::getFramework(const FrameworkID& frameworkId)
{
  return frameworks.registered.contains(frameworkId)
    ? frameworks.registered[frameworkId]
    : NULL;
}


// TODO(bmahler): Consider killing this.
Slave* Master::getSlave(const SlaveID& slaveId)
{
  return slaves.registered.contains(slaveId)
    ? slaves.registered[slaveId]
    : NULL;
}


// TODO(bmahler): Consider killing this.
Offer* Master::getOffer(const OfferID& offerId)
{
  return offers.contains(offerId) ? offers[offerId] : NULL;
}


// Create a new framework ID. We format the ID as MASTERID-FWID, where
// MASTERID is the ID of the master (launch date plus fault tolerant ID)
// and FWID is an increasing integer.
FrameworkID Master::newFrameworkId()
{
  std::ostringstream out;

  out << info_.id() << "-" << std::setw(4)
      << std::setfill('0') << nextFrameworkId++;

  FrameworkID frameworkId;
  frameworkId.set_value(out.str());

  return frameworkId;
}


OfferID Master::newOfferId()
{
  OfferID offerId;
  offerId.set_value(info_.id() + "-O" + stringify(nextOfferId++));
  return offerId;
}


SlaveID Master::newSlaveId()
{
  SlaveID slaveId;
  slaveId.set_value(info_.id() + "-S" + stringify(nextSlaveId++));
  return slaveId;
}


double Master::_slaves_active()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->active) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_inactive()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (!slave->active) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_connected()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (slave->connected) {
      count++;
    }
  }
  return count;
}


double Master::_slaves_disconnected()
{
  double count = 0.0;
  foreachvalue (Slave* slave, slaves.registered) {
    if (!slave->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_connected()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_disconnected()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (!framework->connected) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_active()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (framework->active) {
      count++;
    }
  }
  return count;
}


double Master::_frameworks_inactive()
{
  double count = 0.0;
  foreachvalue (Framework* framework, frameworks.registered) {
    if (!framework->active) {
      count++;
    }
  }
  return count;
}


double Master::_tasks_staging()
{
  double count = 0.0;

  // Add the tasks pending validation / authorization.
  foreachvalue (Framework* framework, frameworks.registered) {
    count += framework->pendingTasks.size();
  }

  foreachvalue (Slave* slave, slaves.registered) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_STAGING) {
          count++;
        }
      }
    }
  }

  return count;
}


double Master::_tasks_starting()
{
  double count = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_STARTING) {
          count++;
        }
      }
    }
  }

  return count;
}


double Master::_tasks_running()
{
  double count = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    typedef hashmap<TaskID, Task*> TaskMap;
    foreachvalue (const TaskMap& tasks, slave->tasks) {
      foreachvalue (const Task* task, tasks) {
        if (task->state() == TASK_RUNNING) {
          count++;
        }
      }
    }
  }

  return count;
}


double Master::_resources_total(const std::string& name)
{
  double total = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreach (const Resource& resource, slave->info.resources()) {
      if (resource.name() == name && resource.type() == Value::SCALAR) {
        total += resource.scalar().value();
      }
    }
  }

  return total;
}


double Master::_resources_used(const std::string& name)
{
  double used = 0.0;

  foreachvalue (Slave* slave, slaves.registered) {
    foreachvalue (const Resources& resources, slave->usedResources) {
      foreach (const Resource& resource, resources) {
        if (resource.name() == name && resource.type() == Value::SCALAR) {
          used += resource.scalar().value();
        }
      }
    }
  }

  return used;
}


double Master::_resources_percent(const std::string& name)
{
  double total = _resources_total(name);

  if (total == 0.0) {
    return total;
  } else {
    return _resources_used(name) / total;
  }
}

} // namespace master {
} // namespace internal {
} // namespace mesos {
