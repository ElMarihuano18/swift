// RUN: %swiftc_driver -driver-print-jobs -sanitize=address -sanitize-address-use-odr-indicator %s 2>&1 | %FileCheck %s

// CHECK: swift
// CHECK-DAG: -sanitize=address
// CHECK-DAG: -sanitize-address-use-odr-indicator
