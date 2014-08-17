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

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <process/help.hpp>
#include <process/owned.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/numify.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "common/attributes.hpp"
#include "common/build.hpp"
#include "common/http.hpp"
#include "common/type_utils.hpp"

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "slave/slave.hpp"


using process::Clock;
using process::DESCRIPTION;
using process::Future;
using process::HELP;
using process::Owned;
using process::TLDR;
using process::USAGE;

using process::http::InternalServerError;
using process::http::OK;

using process::metrics::internal::MetricsProcess;

using std::map;
using std::string;
using std::vector;


namespace mesos {
namespace internal {
namespace slave {


// Pull in defnitions from common.
using mesos::internal::model;

// Pull in the process definitions.
using process::http::Response;
using process::http::Request;


// TODO(bmahler): Kill these in favor of automatic Proto->JSON Conversion (when
// in becomes available).


JSON::Object model(const CommandInfo& command)
{
  JSON::Object object;

  if (command.has_shell()) {
    object.values["shell"] = command.shell();
  }

  if (command.has_value()) {
    object.values["value"] = command.value();
  }

  JSON::Array argv;
  foreach (const string& arg, command.arguments()) {
    argv.values.push_back(arg);
  }
  object.values["argv"] = argv;

  if (command.has_environment()) {
    JSON::Object environment;
    JSON::Array variables;
    foreach(const Environment_Variable& variable,
            command.environment().variables()) {
      JSON::Object variableObject;
      variableObject.values["name"] = variable.name();
      variableObject.values["value"] = variable.value();
      variables.values.push_back(variableObject);
    }
    environment.values["variables"] = variables;
    object.values["environment"] = environment;
  }

  JSON::Array uris;
  foreach(const CommandInfo_URI& uri, command.uris()) {
    JSON::Object uriObject;
    uriObject.values["value"] = uri.value();
    uriObject.values["executable"] = uri.executable();

    uris.values.push_back(uriObject);
  }
  object.values["uris"] = uris;

  return object;
}


JSON::Object model(const ExecutorInfo& executorInfo)
{
  JSON::Object object;
  object.values["executor_id"] = executorInfo.executor_id().value();
  object.values["name"] = executorInfo.name();
  object.values["data"] = executorInfo.data();
  object.values["framework_id"] = executorInfo.framework_id().value();
  object.values["command"] = model(executorInfo.command());
  object.values["resources"] = model(executorInfo.resources());
  return object;
}


JSON::Object model(const TaskInfo& task)
{
  JSON::Object object;
  object.values["id"] = task.task_id().value();
  object.values["name"] = task.name();
  object.values["slave_id"] = task.slave_id().value();
  object.values["resources"] = model(task.resources());
  object.values["data"] = task.data();

  if (task.has_command()) {
    object.values["command"] = model(task.command());
  }
  if (task.has_executor()) {
    object.values["executor_id"] = model(task.executor());
  }

  return object;
}


JSON::Object model(const Executor& executor)
{
  JSON::Object object;
  object.values["id"] = executor.id.value();
  object.values["name"] = executor.info.name();
  object.values["source"] = executor.info.source();
  object.values["container"] = executor.containerId.value();
  object.values["directory"] = executor.directory;

  if (executor.resources.isSome()) {
    object.values["resources"] = model(executor.resources.get());
  }

  JSON::Array tasks;
  foreach (Task* task, executor.launchedTasks.values()) {
    tasks.values.push_back(model(*task));
  }
  object.values["tasks"] = tasks;

  JSON::Array queued;
  foreach (const TaskInfo& task, executor.queuedTasks.values()) {
    queued.values.push_back(model(task));
  }
  object.values["queued_tasks"] = queued;

  JSON::Array completed;
  foreach (const memory::shared_ptr<Task>& task, executor.completedTasks) {
    completed.values.push_back(model(*task));
  }

  // NOTE: We add 'terminatedTasks' to 'completed_tasks' for
  // simplicity.
  // TODO(vinod): Use foreachvalue instead once LinkedHashmap
  // supports it.
  foreach (Task* task, executor.terminatedTasks.values()) {
    completed.values.push_back(model(*task));
  }
  object.values["completed_tasks"] = completed;

  return object;
}


// Returns a JSON object modeled after a Framework.
JSON::Object model(const Framework& framework)
{
  JSON::Object object;
  object.values["id"] = framework.id.value();
  object.values["name"] = framework.info.name();
  object.values["user"] = framework.info.user();
  object.values["failover_timeout"] = framework.info.failover_timeout();
  object.values["checkpoint"] = framework.info.checkpoint();
  object.values["role"] = framework.info.role();
  object.values["hostname"] = framework.info.hostname();

  JSON::Array executors;
  foreachvalue (Executor* executor, framework.executors) {
    executors.values.push_back(model(*executor));
  }
  object.values["executors"] = executors;

  JSON::Array completedExecutors;
  foreach (const Owned<Executor>& executor, framework.completedExecutors) {
    completedExecutors.values.push_back(model(*executor));
  }
  object.values["completed_executors"] = completedExecutors;

  return object;
}


const string Slave::Http::HEALTH_HELP = HELP(
    TLDR(
        "Health check of the Slave."),
    USAGE(
        "/health"),
    DESCRIPTION(
        "Returns 200 OK iff the Slave is healthy.",
        "Delayed responses are also indicative of poor health."));


Future<Response> Slave::Http::health(const Request& request)
{
  return OK();
}


// Declaration of 'stats' continuation.
static Future<Response> _stats(
    const Request& request,
    JSON::Object object,
    const Response& response);


Future<Response> Slave::Http::stats(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["uptime"] = (Clock::now() - slave->startTime).secs();
  object.values["total_frameworks"] = slave->frameworks.size();
  object.values["registered"] = slave->master.isSome() ? "1" : "0";
  object.values["recovery_errors"] = slave->recoveryErrors;

  // NOTE: These are monotonically increasing counters.
  object.values["staged_tasks"] = slave->stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = slave->stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = slave->stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = slave->stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = slave->stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = slave->stats.tasks[TASK_LOST];
  object.values["valid_status_updates"] = slave->stats.validStatusUpdates;
  object.values["invalid_status_updates"] = slave->stats.invalidStatusUpdates;

  // NOTE: These are gauges representing instantaneous values.

  // Queued waiting for executor to register.
  int queued_tasks = 0;

  // Sent to executor (TASK_STAGING, TASK_STARTING, TASK_RUNNING).
  int launched_tasks = 0;

  foreachvalue (Framework* framework, slave->frameworks) {
    foreachvalue (Executor* executor, framework->executors) {
      queued_tasks += executor->queuedTasks.size();
      launched_tasks += executor->launchedTasks.size();
    }
  }

  object.values["queued_tasks_gauge"] = queued_tasks;
  object.values["launched_tasks_gauge"] = launched_tasks;

  // Include metrics from libprocess metrics while we sunset this
  // endpoint in favor of libprocess metrics.
  // TODO(benh): Remove this after transitioning to libprocess metrics.
  return process::http::get(MetricsProcess::instance()->self(), "snapshot")
    .then(lambda::bind(&_stats, request, object, lambda::_1));
}


static Future<Response> _stats(
    const Request& request,
    JSON::Object object,
    const Response& response)
{
  if (response.status != process::http::statuses[200]) {
    return InternalServerError("Failed to get metrics: " + response.status);
  }

  Option<string> type = response.headers.get("Content-Type");

  if (type.isNone() || type.get() != "application/json") {
    return InternalServerError("Failed to get metrics: expecting JSON");
  }

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response.body);

  if (parse.isError()) {
    return InternalServerError("Failed to parse metrics: " + parse.error());
  }

  // Now add all the values from metrics.
  // TODO(benh): Make sure we're not overwriting any values.
  object.values.insert(parse.get().values.begin(), parse.get().values.end());

  return OK(object, request.query.get("jsonp"));
}


Future<Response> Slave::Http::state(const Request& request)
{
  LOG(INFO) << "HTTP request for '" << request.path << "'";

  JSON::Object object;
  object.values["version"] = MESOS_VERSION;

  if (build::GIT_SHA.isSome()) {
    object.values["git_sha"] = build::GIT_SHA.get();
  }

  if (build::GIT_BRANCH.isSome()) {
    object.values["git_branch"] = build::GIT_BRANCH.get();
  }

  if (build::GIT_TAG.isSome()) {
    object.values["git_tag"] = build::GIT_TAG.get();
  }

  object.values["build_date"] = build::DATE;
  object.values["build_time"] = build::TIME;
  object.values["build_user"] = build::USER;
  object.values["start_time"] = slave->startTime.secs();
  object.values["id"] = slave->info.id().value();
  object.values["pid"] = string(slave->self());
  object.values["hostname"] = slave->info.hostname();
  object.values["resources"] = model(slave->resources);
  object.values["attributes"] = model(slave->attributes);
  object.values["staged_tasks"] = slave->stats.tasks[TASK_STAGING];
  object.values["started_tasks"] = slave->stats.tasks[TASK_STARTING];
  object.values["finished_tasks"] = slave->stats.tasks[TASK_FINISHED];
  object.values["killed_tasks"] = slave->stats.tasks[TASK_KILLED];
  object.values["failed_tasks"] = slave->stats.tasks[TASK_FAILED];
  object.values["lost_tasks"] = slave->stats.tasks[TASK_LOST];

  if (slave->master.isSome()) {
    Try<string> masterHostname = net::getHostname(slave->master.get().ip);
    if (masterHostname.isSome()) {
      object.values["master_hostname"] = masterHostname.get();
    }
  }

  if (slave->flags.log_dir.isSome()) {
    object.values["log_dir"] = slave->flags.log_dir.get();
  }

  JSON::Array frameworks;
  foreachvalue (Framework* framework, slave->frameworks) {
    frameworks.values.push_back(model(*framework));
  }
  object.values["frameworks"] = frameworks;

  JSON::Array completedFrameworks;
  foreach (const Owned<Framework>& framework, slave->completedFrameworks) {
    completedFrameworks.values.push_back(model(*framework));
  }
  object.values["completed_frameworks"] = completedFrameworks;

  JSON::Object flags;
  foreachpair (const string& name, const flags::Flag& flag, slave->flags) {
    Option<string> value = flag.stringify(slave->flags);
    if (value.isSome()) {
      flags.values[name] = value.get();
    }
  }
  object.values["flags"] = flags;

  return OK(object, request.query.get("jsonp"));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
