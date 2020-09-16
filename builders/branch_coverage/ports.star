# Copyright 2020 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

load("//lib/lib.star", "GOMA", "branch_coverage_builder", "in_branch_console")

in_category = in_branch_console("ports")

in_category(
    "Arm",
    branch_coverage_builder(
        name = "V8 Arm - builder",
        triggering_policy = scheduler.policy(
            kind = scheduler.GREEDY_BATCHING_KIND,
            max_batch_size = 1,
        ),
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"triggers": ["V8 Arm"], "builder_group": "client.v8.ports", "target_arch": "arm", "build_config": "Release", "binary_size_tracking": {"category": "linux_arm32", "binary": "d8"}},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Arm - debug builder",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Debug", "triggers": ["V8 Arm - debug", "V8 Arm GC Stress"], "target_arch": "arm", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Android Arm - builder",
        triggering_policy = scheduler.policy(
            kind = scheduler.GREEDY_BATCHING_KIND,
            max_batch_size = 1,
        ),
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"builder_group": "client.v8.ports", "target_arch": "arm", "build_config": "Release", "target_platform": "android", "binary_size_tracking": {"category": "android_arm32", "binary": "d8"}},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm - sim",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm - sim - debug",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Debug", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm - sim - lite",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm - sim - lite - debug",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Debug", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Arm",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 28800,
        properties = {"builder_group": "client.v8.ports"},
    ),
    branch_coverage_builder(
        name = "V8 Arm - debug",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 27000,
        properties = {"builder_group": "client.v8.ports"},
    ),
    branch_coverage_builder(
        name = "V8 Arm GC Stress",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 30600,
        properties = {"builder_group": "client.v8.ports"},
    ),
)

in_category(
    "Arm64",
    branch_coverage_builder(
        name = "V8 Arm64 - builder",
        triggering_policy = scheduler.policy(
            kind = scheduler.GREEDY_BATCHING_KIND,
            max_batch_size = 1,
        ),
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "target_arch": "arm", "target_bits": 64, "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Android Arm64 - builder",
        triggering_policy = scheduler.policy(
            kind = scheduler.GREEDY_BATCHING_KIND,
            max_batch_size = 1,
        ),
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"triggers": ["V8 Android Arm64 - N5X"], "builder_group": "client.v8.ports", "target_arch": "arm", "build_config": "Release", "target_platform": "android", "binary_size_tracking": {"category": "android_arm64", "binary": "d8"}},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Android Arm64 - debug builder",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Debug", "target_platform": "android", "target_arch": "arm", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Android Arm64 - N5X",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        properties = {"builder_group": "client.v8.ports"},
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm64 - sim",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm64 - sim - debug",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Debug", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - arm64 - sim - gc stress",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        execution_timeout = 23400,
        properties = {"build_config": "Debug", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux64 - arm64 - sim - pointer compression - builder",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8", "triggers": ["V8 Linux64 - arm64 - sim - pointer compression"]},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux64 - arm64 - sim - pointer compression",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 19800,
        properties = {"builder_group": "client.v8"},
    ),
)

in_category(
    "Mips",
    branch_coverage_builder(
        name = "V8 Linux - mipsel - sim - builder",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8.ports", "triggers": ["V8 Linux - mipsel - sim"]},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - mips64el - sim - builder",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        properties = {"build_config": "Release", "builder_group": "client.v8.ports", "triggers": ["V8 Linux - mips64el - sim"]},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - mipsel - sim",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 19800,
        properties = {"builder_group": "client.v8.ports"},
    ),
    branch_coverage_builder(
        name = "V8 Linux - mips64el - sim",
        triggered_by_gitiles = False,
        dimensions = {"host_class": "multibot"},
        execution_timeout = 19800,
        properties = {"builder_group": "client.v8.ports"},
    ),
)

in_category(
    "IBM",
    branch_coverage_builder(
        name = "V8 Linux - ppc64 - sim",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        execution_timeout = 19800,
        properties = {"build_config": "Release", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
    branch_coverage_builder(
        name = "V8 Linux - s390x - sim",
        triggered_by_gitiles = True,
        dimensions = {"os": "Ubuntu-16.04", "cpu": "x86-64"},
        execution_timeout = 19800,
        properties = {"build_config": "Release", "builder_group": "client.v8.ports"},
        use_goma = GOMA.DEFAULT,
    ),
)