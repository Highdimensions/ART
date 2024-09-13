/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

public class Main {
  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear(long, int) instruction_simplifier_riscv64 (before)
  /// CHECK:          <<A:j\d+>>           ParameterValue
  /// CHECK:          <<B:i\d+>>           ParameterValue
  /// CHECK:          <<One:j\d+>>         LongConstant 1
  /// CHECK:          <<Shift:j\d+>>       Shl [<<One>>,<<B>>]
  /// CHECK:          <<Not:j\d+>>         Not [<<Shift>>]
  /// CHECK:          <<And:j\d+>>         And [<<A>>,<<Not>>]
  /// CHECK:          <<Return:v\d+>>      Return [<<And>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>           ParameterValue
  /// CHECK:          <<B:i\d+>>           ParameterValue
  /// CHECK-DAG:      <<BitClear:j\d+>>    Riscv64BitClear [<<A>>,<<B>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK-NOT:                           Shl
  /// CHECK-NOT:                           Not
  /// CHECK-NOT:                           And

  public static long $noinline$longIntRiscvBitClear(long a, int b) {
    return a & ~(1L << b);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitExtract(long, int) instruction_simplifier_riscv64 (before)
  /// CHECK:          <<A:j\d+>>            ParameterValue
  /// CHECK:          <<B:i\d+>>            ParameterValue
  /// CHECK:          <<One:j\d+>>          LongConstant 1
  /// CHECK:          <<Shift:j\d+>>        Shr [<<A>>,<<B>>]
  /// CHECK:          <<And:j\d+>>          And [<<Shift>>,<<One>>]
  /// CHECK:          <<Return:v\d+>>       Return [<<And>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitExtract(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>            ParameterValue
  /// CHECK:          <<B:i\d+>>            ParameterValue
  /// CHECK-DAG:      <<BitExtract:j\d+>>   Riscv64BitExtract [<<A>>,<<B>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitExtract(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK-NOT:                            Shr
  /// CHECK-NOT:                            And

  public static long $noinline$longIntRiscvBitExtract(long a, int b) {
    return (a >> b) & 1L;
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert(long, int) instruction_simplifier_riscv64 (before)
  /// CHECK:          <<A:j\d+>>           ParameterValue
  /// CHECK:          <<B:i\d+>>           ParameterValue
  /// CHECK:          <<One:j\d+>>         LongConstant 1
  /// CHECK:          <<Shift:j\d+>>       Shl [<<One>>,<<B>>]
  /// CHECK:          <<Xor:j\d+>>         Xor [<<A>>,<<Shift>>]
  /// CHECK:          <<Return:v\d+>>      Return [<<Xor>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>           ParameterValue
  /// CHECK:          <<B:i\d+>>           ParameterValue
  /// CHECK-DAG:      <<BitInvert:j\d+>>   Riscv64BitInvert [<<A>>,<<B>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK-NOT:                           Shl
  /// CHECK-NOT:                           Xor

  public static long $noinline$longIntRiscvBitInvert(long a, int b) {
    return a ^ (1L << b);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet(long, int) instruction_simplifier_riscv64 (before)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK:          <<B:i\d+>>        ParameterValue
  /// CHECK:          <<One:j\d+>>      LongConstant 1
  /// CHECK:          <<Shift:j\d+>>    Shl [<<One>>,<<B>>]
  /// CHECK:          <<Or:j\d+>>       Or [<<A>>,<<Shift>>]
  /// CHECK:          <<Return:v\d+>>   Return [<<Or>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK:          <<B:i\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitSet:j\d+>>   Riscv64BitSet [<<A>>,<<B>>]

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet(long, int) instruction_simplifier_riscv64 (after)
  /// CHECK-NOT:                        Shl
  /// CHECK-NOT:                        Or

  public static long $noinline$longIntRiscvBitSet(long a, int b) {
    return a | (1L << b);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet35(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitSet:j\d+>>   Riscv64BitSet [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitSet35(long a) {
    return a | (1L << 35L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet45(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitSet:j\d+>>   Riscv64BitSet [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitSet45(long a) {
    return a | (1L << 45L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitSet55(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitSet:j\d+>>   Riscv64BitSet [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitSet55(long a) {
    return a | (1L << 55L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert35(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitInvert:j\d+>> Riscv64BitInvert [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitInvert35(long a) {
    return a ^ (1L << 35L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert45(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitInvert:j\d+>> Riscv64BitInvert [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitInvert45(long a) {
    return a ^ (1L << 45L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitInvert55(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitInvert:j\d+>> Riscv64BitInvert [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitInvert55(long a) {
    return a ^ (1L << 55L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear35(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitClear:j\d+>> Riscv64BitClear [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitClear35(long a) {
    return a & ~(1L << 35L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear45(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitClear:j\d+>> Riscv64BitClear [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitClear45(long a) {
    return a & ~(1L << 45L);
  }

  /// CHECK-START-RISCV64: long Main.$noinline$longIntRiscvBitClear40(long) instruction_simplifier_riscv64 (after)
  /// CHECK:          <<A:j\d+>>        ParameterValue
  /// CHECK-DAG:      <<BitClear:j\d+>> Riscv64BitClear [<<A>>,{{i\d+}}]

  public static long $noinline$longIntRiscvBitClear40(long a) {
    return a & ~(1L << 40L);
  }

  public static void main(String[] args) {
    assertLongEquals(0L, $noinline$longIntRiscvBitClear(2L, 1));
    assertLongEquals(1109L, $noinline$longIntRiscvBitClear(1111L, 1));
    assertLongEquals(32768L, $noinline$longIntRiscvBitClear(32768L, 13));
    assertLongEquals(0x7FFFFFFFFFFFFFFFL, $noinline$longIntRiscvBitClear(0xFFFFFFFFFFFFFFFFL, 63));
    assertLongEquals(0L, $noinline$longIntRiscvBitClear(1L << 35, 35));
    assertLongEquals(~(1L << 35), $noinline$longIntRiscvBitClear(~0L, 35));
    assertLongEquals(122L, $noinline$longIntRiscvBitClear(123L, 64));
    assertLongEquals(0L, $noinline$longIntRiscvBitClear(Long.MIN_VALUE, 63));

    assertLongEquals(1200208152866L,$noinline$longIntRiscvBitClear35(1234567891234L));
    assertLongEquals(5298723004898717825L,$noinline$longIntRiscvBitClear40(5298724104410345601L));
    assertLongEquals(52952056672014624L,$noinline$longIntRiscvBitClear45(52987241044103456L));

    assertLongEquals(0L, $noinline$longIntRiscvBitExtract(10L, 2));
    assertLongEquals(0L, $noinline$longIntRiscvBitExtract(1024L, 8));
    assertLongEquals(1L, $noinline$longIntRiscvBitExtract(7L, 1));
    assertLongEquals(1L, $noinline$longIntRiscvBitExtract(1L << 62, 62));
    assertLongEquals(1L, $noinline$longIntRiscvBitExtract(1L, 64));
    assertLongEquals(0L, $noinline$longIntRiscvBitExtract(2L, 64));

    assertLongEquals(2L, $noinline$longIntRiscvBitInvert(10L, 3));
    assertLongEquals(3808L, $noinline$longIntRiscvBitInvert(12000L, 13));
    assertLongEquals(40960L, $noinline$longIntRiscvBitInvert(32768L, 13));
    assertLongEquals(0L, $noinline$longIntRiscvBitInvert(Long.MIN_VALUE, 63));
    assertLongEquals(Long.MIN_VALUE, $noinline$longIntRiscvBitInvert(0L, 63));
    assertLongEquals(0L, $noinline$longIntRiscvBitInvert(1L, 64));
    assertLongEquals(3L, $noinline$longIntRiscvBitInvert(2L, 64));

    assertLongEquals(34359739602L, $noinline$longIntRiscvBitInvert35(1234L));
    assertLongEquals(35184372141674L, $noinline$longIntRiscvBitInvert45(52842L));
    assertLongEquals(36028810166058116L, $noinline$longIntRiscvBitInvert55(13147094148L));

    assertLongEquals(101L, $noinline$longIntRiscvBitSet(100L, 0));
    assertLongEquals(116L, $noinline$longIntRiscvBitSet(100L, 4));
    assertLongEquals(612L, $noinline$longIntRiscvBitSet(100L, 9));
    assertLongEquals(101L, $noinline$longIntRiscvBitSet(100L, 0));
    assertLongEquals(Long.MIN_VALUE, $noinline$longIntRiscvBitSet(0L, 63));
    assertLongEquals(1L, $noinline$longIntRiscvBitSet(0L, 64));

    assertLongEquals(34359739368L, $noinline$longIntRiscvBitSet35(1000L));
    assertLongEquals(35184372089832L, $noinline$longIntRiscvBitSet45(1000L));
    assertLongEquals(36028797018964968L, $noinline$longIntRiscvBitSet55(1000L));
  }
}
