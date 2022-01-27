// RUN: vast-cc --ccopts -xc --from-source %s | FileCheck %s
// RUN: vast-cc --ccopts -xc --from-source %s > %t && vast-opt %t | diff -B %t -

// CHECK: hl.enum.decl @color : !hl.int<unsigned>
// CHECK:  hl.enum.const @RED = 0 : si32
// CHECK:  hl.enum.const @GREEN = 1 : si32
// CHECK:  hl.enum.const @BLUE = 2 : si32
enum color { RED, GREEN, BLUE };

// CHECK: hl.global @c : !hl.named_type<@color>
// CHECK: [[V1:%[0-9]+]] = hl.declref @GREEN : !hl.int
// CHECK: [[V2:%[0-9]+]] = hl.implicit_cast [[V1]] IntegralCast : !hl.int -> !hl.named_type<@color>
// CHECK: hl.value.yield [[V2]] : !hl.named_type<@color>
enum color c = GREEN;