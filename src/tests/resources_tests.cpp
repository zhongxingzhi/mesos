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

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>

#include "master/master.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::master;
using namespace mesos::internal::tests;

using std::ostringstream;
using std::pair;
using std::string;


TEST(ResourcesTest, Parsing)
{
  Resource cpus = Resources::parse("cpus", "45.55", "*").get();

  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());

  Resource ports = Resources::parse(
      "ports", "[10000-20000, 30000-50000]", "*").get();

  ASSERT_EQ(Value::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  Resource disks = Resources::parse("disks", "{sda1}", "*").get();

  ASSERT_EQ(Value::SET, disks.type());
  ASSERT_EQ(1, disks.set().item_size());
  EXPECT_EQ("sda1", disks.set().item(0));

  Resources r1 = Resources::parse(
      "cpus:45.55;ports:[10000-20000, 30000-50000];disks:{sda1}").get();

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ParsingWithRoles)
{
  Resources parse1 = Resources::parse("cpus(role1):2;mem(role1):3").get();

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);
  cpus.set_role("role1");

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Value::SCALAR);
  mem.mutable_scalar()->set_value(3);
  mem.set_role("role1");

  Resources resources1;
  resources1 += cpus;
  resources1 += mem;

  EXPECT_EQ(parse1, resources1);
  EXPECT_EQ(resources1, Resources::parse(stringify(resources1)).get());

  Resources parse2 = Resources::parse(
      "cpus(role1):2.5;ports(role2):[0-100]").get();

  Resource cpus2;
  cpus2.set_name("cpus");
  cpus2.set_type(Value::SCALAR);
  cpus2.mutable_scalar()->set_value(2.5);
  cpus2.set_role("role1");

  Resource ports;
  ports.set_name("ports");
  ports.set_type(Value::RANGES);
  Value::Range* range = ports.mutable_ranges()->add_range();
  range->set_begin(0);
  range->set_end(100);
  ports.set_role("role2");

  Resources resources2;
  resources2 += ports;
  resources2 += cpus2;

  EXPECT_EQ(parse2, resources2);
  EXPECT_EQ(resources2, Resources::parse(stringify(resources2)).get());

  Resources parse3 = Resources::parse(
      "cpus:2.5;ports(role2):[0-100]", "role1").get();

  EXPECT_EQ(parse2, parse3);
}


TEST(ResourcesTest, ParseError)
{
  // Missing colon.
  EXPECT_ERROR(Resources::parse("cpus(role1)"));

  // Mismatched parentheses.
  EXPECT_ERROR(Resources::parse("cpus(role1:1"));
  EXPECT_ERROR(Resources::parse("cpus)(role1:1"));
}


TEST(ResourcesTest, Resources)
{
  Resources r = Resources::parse(
      "cpus:45.55;mem:1024;ports:[10000-20000, 30000-50000];disk:512").get();

  EXPECT_SOME_EQ(45.55, r.cpus());
  EXPECT_SOME_EQ(Megabytes(1024), r.mem());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());

  ASSERT_SOME(r.ports());

  ostringstream ports;
  ports << r.ports().get();

  EXPECT_EQ("[10000-20000, 30000-50000]", ports.str());

  r = Resources::parse("cpus:45.55;disk:512").get();

  EXPECT_SOME_EQ(45.55, r.cpus());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());
  EXPECT_TRUE(r.mem().isNone());
  EXPECT_TRUE(r.ports().isNone());
}


TEST(ResourcesTest, Printing)
{
  Resources r = Resources::parse(
      "cpus:45.55;ports:[10000-20000, 30000-50000];disks:{sda1}").get();

  string output =
    "cpus(*):45.55; ports(*):[10000-20000, 30000-50000]; disks(*):{sda1}";

  ostringstream oss;
  oss << r;

  // TODO(benh): This test is a bit strict because it implies the
  // ordering of things (e.g., the ordering of resources and the
  // ordering of ranges). We should really just be checking for the
  // existance of certain substrings in the output.

  EXPECT_EQ(output, oss.str());
}


TEST(ResourcesTest, InitializedIsEmpty)
{
  Resources r;
  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, BadResourcesNotAllocatable)
{
  Resource cpus;
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(1);

  Resources r;
  r += cpus;

  EXPECT_TRUE(r.empty());

  cpus.set_name("cpus");
  cpus.mutable_scalar()->set_value(0);

  r += cpus;

  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, ScalarEquals)
{
  Resource cpus = Resources::parse("cpus", "3", "*").get();
  Resource mem =  Resources::parse("mem", "3072", "*").get();

  Resources r1;
  r1 += cpus;
  r1 += mem;

  Resources r2;
  r2 += cpus;
  r2 += mem;

  EXPECT_FALSE(r1.empty());
  EXPECT_FALSE(r2.empty());
  EXPECT_EQ(r1, r2);

  Resources cpus1 = Resources::parse("cpus", "3", "role1").get();
  Resources cpus2 = Resources::parse("cpus", "3", "role2").get();

  EXPECT_NE(cpus1, cpus2);
}


TEST(ResourcesTest, ScalarSubset)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1 =  Resources::parse("mem", "3072", "*").get();

  Resource cpus2 = Resources::parse("cpus", "1", "*").get();
  Resource mem2 =  Resources::parse("mem", "4096", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
}


TEST(ResourcesTest, ScalarSubset2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "1", "role2").get();

  Resources r1;
  r1 += cpus1;

  Resources r2;
  r2 += cpus2;

  EXPECT_FALSE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));

  Resource cpus3 = Resources::parse("cpus", "3", "role1").get();

  Resources r3;
  r3 += cpus3;

  EXPECT_FALSE(r1.contains(r3));
  EXPECT_FALSE(r2.contains(r3));
  EXPECT_FALSE(r3.contains(r2));
  EXPECT_TRUE(r3.contains(r1));
}


TEST(ResourcesTest, ScalarAddition)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1 = Resources::parse("mem", "5", "*").get();

  Resource cpus2 = Resources::parse("cpus", "2", "*").get();
  Resource mem2 = Resources::parse("mem", "10", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources sum = r1 + r2;

  EXPECT_FALSE(sum.empty());
  EXPECT_EQ(3, sum.get<Value::Scalar>("cpus").get().value());
  EXPECT_EQ(15, sum.get<Value::Scalar>("mem").get().value());

  Resources r = r1;
  r += r2;

  EXPECT_FALSE(r.empty());
  EXPECT_EQ(3, r.get<Value::Scalar>("cpus").get().value());
  EXPECT_EQ(15, r.get<Value::Scalar>("mem").get().value());
}


TEST(ResourcesTest, ScalarAddition2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "5", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += cpus2;

  Resources r2;
  r2 += cpus3;

  Resources sum = r1 + r2;

  EXPECT_FALSE(sum.empty());
  EXPECT_EQ(9, sum.cpus().get());
  EXPECT_EQ(sum, Resources::parse("cpus(role1):6;cpus(role2):3").get());
}


TEST(ResourcesTest, ScalarSubtraction)
{
  Resource cpus1 = Resources::parse("cpus", "50", "*").get();
  Resource mem1 = Resources::parse("mem", "4096", "*").get();

  Resource cpus2 = Resources::parse("cpus", "0.5", "*").get();
  Resource mem2 = Resources::parse("mem", "1024", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources diff = r1 - r2;

  EXPECT_FALSE(diff.empty());
  EXPECT_EQ(49.5, diff.get<Value::Scalar>("cpus").get().value());
  EXPECT_EQ(3072, diff.get<Value::Scalar>("mem").get().value());

  Resources r = r1;
  r -= r2;

  EXPECT_EQ(49.5, diff.get<Value::Scalar>("cpus").get().value());
  EXPECT_EQ(3072, diff.get<Value::Scalar>("mem").get().value());

  r = r1;
  r -= r1;

  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, ScalarSubtraction2)
{
  Resource cpus1 = Resources::parse("cpus", "5", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "1", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += cpus2;

  Resources r2;
  r2 += cpus3;

  Resources diff = r1 - r2;

  EXPECT_FALSE(diff.empty());
  EXPECT_EQ(7, diff.cpus().get());
  EXPECT_EQ(diff, Resources::parse("cpus(role1):4;cpus(role2):3").get());
}


TEST(ResourcesTest, RangesEquals)
{
  Resource ports1 = Resources::parse(
      "ports", "[20-40]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[20-30, 31-39, 40-40]", "*").get();

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, RangesSubset)
{
  Resource ports1 = Resources::parse("ports", "[2-2, 4-5]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports3 = Resources::parse("ports", "[2-3]", "*").get();
  Resource ports4 = Resources::parse("ports", "[1-2, 4-6]", "*").get();
  Resource ports5 = Resources::parse("ports", "[1-4, 5-5]", "*").get();

  EXPECT_EQ(2, ports1.ranges().range_size());
  EXPECT_EQ(1, ports2.ranges().range_size());
  EXPECT_EQ(1, ports3.ranges().range_size());
  EXPECT_EQ(2, ports4.ranges().range_size());
  EXPECT_EQ(2, ports5.ranges().range_size());

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  Resources r3;
  r3 += ports3;

  Resources r4;
  r4 += ports4;

  Resources r5;
  r5 += ports5;

  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
  EXPECT_FALSE(r3.contains(r1));
  EXPECT_FALSE(r1.contains(r3));
  EXPECT_TRUE(r2.contains(r3));
  EXPECT_FALSE(r3.contains(r2));
  EXPECT_TRUE(r4.contains(r1));
  EXPECT_TRUE(r2.contains(r4));
  EXPECT_TRUE(r5.contains(r1));
  EXPECT_FALSE(r1.contains(r5));
}


TEST(ResourcesTest, RangesAddition)
{
  Resource ports1 = Resources::parse(
      "ports", "[20000-40000]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[30000-50000, 10000-20000]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[10000-50000]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAddition2)
{
  Resource ports1 = Resources::parse("ports", "[1-10, 5-30, 50-60]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-65, 70-80]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-65, 70-80]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAdditon3)
{
  Resource ports1 = Resources::parse("ports", "[1-2]", "*").get();
  Resource ports2 = Resources::parse("ports", "[3-4]", "*").get();
  Resource ports3 = Resources::parse("ports", "[7-8]", "*").get();
  Resource ports4 = Resources::parse("ports", "[5-6]", "*").get();

  Resources r1;
  r1 += ports1;
  r1 += ports2;

  EXPECT_FALSE(r1.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-4]").get().ranges(),
      r1.get<Value::Ranges>("ports"));

  Resources r2;
  r2 += ports3;
  r2 += ports4;

  EXPECT_FALSE(r2.empty());

  EXPECT_SOME_EQ(
      values::parse("[5-8]").get().ranges(),
      r2.get<Value::Ranges>("ports"));

  r2 += r1;

  EXPECT_FALSE(r2.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-8]").get().ranges(),
      r2.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAddition4)
{
  Resource ports1 = Resources::parse(
      "ports", "[1-4, 9-10, 20-22, 26-30]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[5-8, 23-25]", "*").get();

  Resources r;
  r += ports1;
  r += ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-10, 20-30]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction)
{
  Resource ports1 = Resources::parse(
      "ports", "[20000-40000]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[10000-20000, 30000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[20001-29999]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction1)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50001]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[50002-60000]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction2)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[50001-60000]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction3)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered = Resources::parse("").get();
  Resources resourcesInUse = Resources::parse("ports:[50000-50001]").get();

  Resources resourcesFree = resources - (resourcesOffered + resourcesInUse);

  EXPECT_FALSE(resourcesFree.empty());

  EXPECT_SOME_EQ(
      values::parse("[50002-60000]").get().ranges(),
      resourcesFree.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction4)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered;
  resourcesOffered += resources;
  resourcesOffered -= resources;

  EXPECT_TRUE(resourcesOffered.empty());
  EXPECT_NONE(resourcesOffered.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction5)
{
  Resource ports1 = Resources::parse(
      "ports", "[1-10, 20-30, 40-50]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[2-9, 15-45, 48-50]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-1, 10-10, 46-47]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction6)
{
  Resource ports1 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports2 = Resources::parse("ports", "[11-20]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-10]").get().ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, SetEquals)
{
  Resource disks = Resources::parse("disks", "{sda1}", "*").get();

  Resources r1;
  r1 += disks;

  Resources r2;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, SetSubset)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda1,sda3,sda4,sda2}", "*").get();

  Resources r1;
  r1 += disks1;

  Resources r2;
  r2 += disks2;

  EXPECT_FALSE(r1.empty());
  EXPECT_FALSE(r2.empty());
  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
}


TEST(ResourcesTest, SetAddition)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2,sda3}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda1,sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r += disks2;

  EXPECT_FALSE(r.empty());

  Option<Value::Set> set = r.get<Value::Set>("disks");

  ASSERT_SOME(set);
  EXPECT_EQ(4, set.get().item_size());
}


TEST(ResourcesTest, SetSubtraction)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2,sda3,sda4}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r -= disks2;

  EXPECT_FALSE(r.empty());

  Option<Value::Set> set = r.get<Value::Set>("disks");

  ASSERT_SOME(set);
  EXPECT_EQ(1, set.get().item_size());
  EXPECT_EQ("sda1", set.get().item(0));
}


TEST(ResourcesTest, EmptyUnequal)
{
  Resources empty = Resources::parse("").get();
  Resources cpus2 = Resources::parse("cpus:2").get();

  EXPECT_FALSE(empty == cpus2);
}


TEST(ResourcesTest, Reservations)
{
  Resources unreserved = Resources::parse(
      "cpus:1;mem:2;disk:4").get();
  Resources role1 = Resources::parse(
      "cpus(role1):2;mem(role1):4;disk(role1):8;").get();
  Resources role2 = Resources::parse(
      "cpus(role2):4;mem(role2):8;disk(role2):6;").get();

  Resources resources = unreserved + role1 + role2;

  hashmap<string, Resources> reserved = resources.reserved();

  EXPECT_EQ(2u, reserved.size());
  EXPECT_EQ(role1, reserved["role1"]);
  EXPECT_EQ(role2, reserved["role2"]);

  EXPECT_EQ(role1, resources.reserved("role1"));
  EXPECT_EQ(role2, resources.reserved("role2"));

  // Resources with role "*" are not considered reserved.
  EXPECT_EQ(Resources(), resources.reserved("*"));

  EXPECT_EQ(unreserved, resources.unreserved());
}


TEST(ResourcesTest, FlattenRoles)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "2", "role2").get();
  Resource mem1 = Resources::parse("mem", "5", "role1").get();

  Resources r;
  r += cpus1;
  r += cpus2;
  r += mem1;

  EXPECT_EQ(r.flatten(), Resources::parse("cpus:3;mem:5").get());
}


TEST(ResourcesTest, Find)
{
  Resources resources1 = Resources::parse(
      "cpus(role1):2;mem(role1):10;cpus:4;mem:20").get();

  Resources targets1 = Resources::parse(
      "cpus(role1):3;mem(role1):15").get();

  EXPECT_SOME_EQ(
      Resources::parse("cpus(role1):2;mem(role1):10;cpus:1;mem:5").get(),
      resources1.find(targets1));

  Resources resources2 = Resources::parse(
      "cpus(role1):1;mem(role1):5;cpus(role2):2;"
      "mem(role2):8;cpus:1;mem:7").get();

  Resources targets2 = Resources::parse(
      "cpus(role1):3;mem(role1):15").get();

  EXPECT_SOME_EQ(
      Resources::parse(
        "cpus(role1):1;mem(role1):5;cpus:1;mem:7;"
        "cpus(role2):1;mem(role2):3").get(),
      resources2.find(targets2));

  Resources resources3 = Resources::parse(
      "cpus(role1):5;mem(role1):5;cpus:5;mem:5").get();

  Resources targets3 = Resources::parse("cpus:6;mem:6").get();

  EXPECT_SOME_EQ(
      Resources::parse("cpus:5;mem:5;cpus(role1):1;mem(role1):1").get(),
      resources3.find(targets3));

  Resources resources4 = Resources::parse("cpus(role1):1;mem(role1):1").get();
  Resources targets4 = Resources::parse("cpus(role1):2;mem(role1):2").get();

  EXPECT_NONE(resources4.find(targets4));
}


// Helper for creating a disk resource.
static Resource createDiskResource(
    const string& value,
    const string& role,
    const Option<string>& persistenceID,
    const Option<string>& containerPath)
{
  Resource resource = Resources::parse("disk", value, role).get();

  if (persistenceID.isSome() || containerPath.isSome()) {
    resource.mutable_disk()->CopyFrom(
        createDiskInfo(persistenceID, containerPath));
  }

  return resource;
}


TEST(DiskResourcesTest, Validation)
{
  Resource cpus = Resources::parse("cpus", "2", "*").get();
  cpus.mutable_disk()->CopyFrom(createDiskInfo("1", "path"));

  Option<Error> error = Resources::validate(cpus);
  ASSERT_SOME(error);
  EXPECT_EQ(
      "DiskInfo should not be set for cpus resource",
      error.get().message);

  EXPECT_NONE(
      Resources::validate(createDiskResource("10", "role", "1", "path")));

  EXPECT_NONE(
      Resources::validate(createDiskResource("10", "*", None(), "path")));
}


TEST(DiskResourcesTest, Equals)
{
  Resources r1 = createDiskResource("10", "*", None(), None());
  Resources r2 = createDiskResource("10", "*", None(), "path1");
  Resources r3 = createDiskResource("10", "*", None(), "path2");
  Resources r4 = createDiskResource("10", "role", None(), "path2");
  Resources r5 = createDiskResource("10", "role", "1", "path1");
  Resources r6 = createDiskResource("10", "role", "1", "path2");
  Resources r7 = createDiskResource("10", "role", "2", "path2");

  EXPECT_EQ(r1, r2);
  EXPECT_EQ(r2, r3);
  EXPECT_EQ(r5, r6);

  EXPECT_NE(r6, r7);
  EXPECT_NE(r4, r7);
}


TEST(DiskResourcesTest, Addition)
{
  Resources r1 = createDiskResource("10", "role", None(), "path");
  Resources r2 = createDiskResource("10", "role", None(), None());
  Resources r3 = createDiskResource("20", "role", None(), "path");

  EXPECT_EQ(r3, r1 + r2);

  Resources r4 = createDiskResource("10", "role", "1", "path");
  Resources r5 = createDiskResource("10", "role", "2", "path");
  Resources r6 = createDiskResource("20", "role", "1", "path");

  Resources sum = r4 + r5;

  EXPECT_TRUE(sum.contains(r4));
  EXPECT_TRUE(sum.contains(r5));
  EXPECT_FALSE(sum.contains(r3));
  EXPECT_FALSE(sum.contains(r6));
}


TEST(DiskResourcesTest, Subtraction)
{
  Resources r1 = createDiskResource("10", "role", None(), "path");
  Resources r2 = createDiskResource("10", "role", None(), None());

  EXPECT_TRUE((r1 - r2).empty());

  Resources r3 = createDiskResource("10", "role", "1", "path");
  Resources r4 = createDiskResource("10", "role", "2", "path");
  Resources r5 = createDiskResource("10", "role", "2", "path2");

  EXPECT_EQ(r3, r3 - r4);
  EXPECT_TRUE((r3 - r3).empty());
  EXPECT_TRUE((r4 - r5).empty());
}


TEST(DiskResourcesTest, FilterPersistentDisks)
{
  Resources resources = Resources::parse("cpus:1;mem:512;disk:1000").get();

  Resources disk1 = createDiskResource("10", "role1", "1", "path");
  Resources disk2 = createDiskResource("20", "role2", None(), None());

  resources += disk1;
  resources += disk2;

  EXPECT_EQ(resources.persistentDisks(), disk1);
}


TEST(ResourcesTransformationTest, AcquirePersistentDisk)
{
  Resources total = Resources::parse("cpus:1;mem:512;disk(role):1000").get();

  Resource disk1 = createDiskResource("200", "role", "1", "path");
  Resources::AcquirePersistentDisk acquire1(disk1);

  EXPECT_SOME_EQ(
      Resources::parse("cpus:1;mem:512;disk(role):800").get() + disk1,
      acquire1(total));

  // Check the case of insufficient disk resources.
  Resource disk2 = createDiskResource("2000", "role", "1", "path");
  Resources::AcquirePersistentDisk acquire2(disk2);

  EXPECT_ERROR(acquire2(total));
}
