---
layout: documentation
---

# Upgrading Mesos

This document serves as a guide for users who wish to upgrade an existing mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

## (WIP) Upgrading from 0.21.x to 0.22.x

**NOTE**: The Authentication API has changed slightly in this release to support additional authentication mechanisms. The change from 'string' to 'bytes' for AuthenticationStartMessage.data has no impact on C++ or the over-the-wire representation, so it only impacts pure language bindings for languages like Java and Python that use different types for UTF-8 strings vs. byte arrays.

```
message AuthenticationStartMessage {
  required string mechanism = 1;
  optional bytes data = 2;
}
```


## Upgrading from 0.20.x to 0.21.x

**NOTE** Disabling slave checkpointing has been deprecated; the slave --checkpoint flag has been deprecated and will be removed in a future release.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.19.x to 0.20.x.

**NOTE**: The Mesos API has been changed slightly in this release. The CommandInfo has been changed (see below), which makes launching a command more flexible. The 'value' field has been changed from _required_ to _optional_. However, it will not cause any issue during the upgrade (since the existing schedulers always set this field).

```
message CommandInfo {
  ...
  // There are two ways to specify the command:
  // 1) If 'shell == true', the command will be launched via shell
  //    (i.e., /bin/sh -c 'value'). The 'value' specified will be
  //    treated as the shell command. The 'arguments' will be ignored.
  // 2) If 'shell == false', the command will be launched by passing
  //    arguments to an executable. The 'value' specified will be
  //    treated as the filename of the executable. The 'arguments'
  //    will be treated as the arguments to the executable. This is
  //    similar to how POSIX exec families launch processes (i.e.,
  //    execlp(value, arguments(0), arguments(1), ...)).
  optional bool shell = 6 [default = true];
  optional string value = 3;
  repeated string arguments = 7;
  ...
}
```

**NOTE**: The Python bindings are also changing in this release. There are now sub-modules which allow you to use either the interfaces and/or the native driver.

* `import mesos.native` for the native drivers
* `import mesos.interface` for the stub implementations and protobufs

To ensure a smooth upgrade, we recommend to upgrade your python framework and executor first. You will be able to either import using the new configuration or the old. Replace the existing imports with something like the following:

```
    try:
        from mesos.native import MesosExecutorDriver, MesosSchedulerDriver
        from mesos.interface import Executor, Scheduler
        from mesos.interface import mesos_pb2
    except ImportError:
        from mesos import Executor, MesosExecutorDriver, MesosSchedulerDriver, Scheduler
        import mesos_pb2
```

**NOTE**: If you're using a pure language binding, please ensure that it sends status update acknowledgements through the master before upgrading.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (install the latest mesos jar and python egg if necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library (install the latest mesos jar and python egg if necessary).

## Upgrading from 0.18.x to 0.19.x.

**NOTE**: There are new required flags on the master (`--work_dir` and `--quorum`) to support the *Registrar* feature, which adds replicated state on the masters.

**NOTE**: No required upgrade ordering across components.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Install the new slave binaries and restart the slaves.
* Upgrade the schedulers by linking the latest native library (mesos jar upgrade not necessary).
* Restart the schedulers.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.17.0 to 0.18.x.

In order to upgrade a running cluster:

**NOTE**: This upgrade requires a system reboot for slaves that use Linux cgroups for isolation.

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries then perform one of the following two steps, depending on if cgroups isolation is used:
  * [no cgroups]
      - Restart the slaves. The "--isolation" flag has changed and "process" has been deprecated in favor of "posix/cpu,posix/mem".
  * [cgroups]
      - Change from a single mountpoint for all controllers to separate mountpoints for each controller, e.g., /sys/fs/cgroup/memory/ and /sys/fs/cgroup/cpu/.
      - The suggested configuration is to mount a tmpfs filesystem to /sys/fs/cgroup and to let the slave mount the required controllers. However, the slave will also use previously mounted controllers if they are appropriately mounted under "--cgroups_hierarchy".
      - It has been observed that unmounting and remounting of cgroups from the single to separate configuration is unreliable and a reboot into the new configuration is strongly advised. Restart the slaves after reboot.
      - The "--cgroups_hierarchy" now defaults to "/sys/fs/cgroup". The "--cgroups_root" flag default remains "mesos".
      -  The "--isolation" flag has changed and "cgroups" has been deprecated in favor of "cgroups/cpu,cgroups/mem".
      - The "--cgroup_subsystems" flag is no longer required and will be ignored.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.16.0 to 0.17.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.15.0 to 0.16.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).


## Upgrading from 0.14.0 to 0.15.0.

In order to upgrade a running cluster:

* Install the new master binaries.
* Restart the masters with --credentials pointing to credentials of the framework(s).
* NOTE: --authentication=false (default) allows both authenticated and unauthenticated frameworks to register.
* Install the new slave binaries and restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* NOTE: Schedulers should implement the new `reconcileTasks` driver method.
* Schedulers should call the new `MesosSchedulerDriver` constructor that takes `Credential` to authenticate.
* Restart the schedulers.
* Restart the masters with --authentication=true.
* NOTE: After the restart unauthenticated frameworks *will not* be allowed to register.


## Upgrading from 0.13.0 to 0.14.0.

In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* NOTE: /vars endpoint has been removed.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).
* Install the new slave binaries.
* Restart the slaves after adding --checkpoint flag to enable checkpointing.
* NOTE: /vars endpoint has been removed.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Set FrameworkInfo.checkpoint in the scheduler if checkpointing is desired (recommended).
* Restart the schedulers.
* Restart the masters (to get rid of the cached FrameworkInfo).
* Restart the slaves (to get rid of the cached FrameworkInfo).

## Upgrading from 0.12.0 to 0.13.0.
In order to upgrade a running cluster:

* Install the new master binaries and restart the masters.
* Upgrade the schedulers by linking the latest native library and mesos jar (if necessary).
* Restart the schedulers.
* Install the new slave binaries.
* NOTE: cgroups_hierarchy_root slave flag is renamed as cgroups_hierarchy
* Restart the slaves.
* Upgrade the executors by linking the latest native library and mesos jar (if necessary).

## Upgrading from 0.11.0 to 0.12.0.
In order to upgrade a running cluster:

* Install the new slave binaries and restart the slaves.
* Install the new master binaries and restart the masters.

If you are a framework developer, you will want to examine the new 'source' field in the ExecutorInfo protobuf. This will allow you to take further advantage of the resource monitoring.
