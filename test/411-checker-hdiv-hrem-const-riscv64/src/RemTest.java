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

public class RemTest {
  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void main() {
    remInt();
    remLong();
  }

  private static void remInt() {
    expectEquals(0, $noinline$IntRemBy18(0));
    expectEquals(1, $noinline$IntRemBy18(1));
    expectEquals(-1, $noinline$IntRemBy18(-1));
    expectEquals(0, $noinline$IntRemBy18(18));
    expectEquals(0, $noinline$IntRemBy18(-18));
    expectEquals(11, $noinline$IntRemBy18(65));
    expectEquals(-11, $noinline$IntRemBy18(-65));

    expectEquals(0, $noinline$IntALenRemBy18(new int[0]));
    expectEquals(1, $noinline$IntALenRemBy18(new int[1]));
    expectEquals(0, $noinline$IntALenRemBy18(new int[18]));
    expectEquals(11, $noinline$IntALenRemBy18(new int[65]));

    expectEquals(0, $noinline$IntRemByMinus18(0));
    expectEquals(1, $noinline$IntRemByMinus18(1));
    expectEquals(-1, $noinline$IntRemByMinus18(-1));
    expectEquals(0, $noinline$IntRemByMinus18(18));
    expectEquals(0, $noinline$IntRemByMinus18(-18));
    expectEquals(11, $noinline$IntRemByMinus18(65));
    expectEquals(-11, $noinline$IntRemByMinus18(-65));

    expectEquals(0, $noinline$IntRemBy7(0));
    expectEquals(1, $noinline$IntRemBy7(1));
    expectEquals(-1, $noinline$IntRemBy7(-1));
    expectEquals(0, $noinline$IntRemBy7(7));
    expectEquals(0, $noinline$IntRemBy7(-7));
    expectEquals(1, $noinline$IntRemBy7(22));
    expectEquals(-1, $noinline$IntRemBy7(-22));

    expectEquals(0, $noinline$IntALenRemBy7(new int[0]));
    expectEquals(1, $noinline$IntALenRemBy7(new int[1]));
    expectEquals(0, $noinline$IntALenRemBy7(new int[7]));
    expectEquals(1, $noinline$IntALenRemBy7(new int[22]));

    expectEquals(0, $noinline$IntRemByMinus7(0));
    expectEquals(1, $noinline$IntRemByMinus7(1));
    expectEquals(-1, $noinline$IntRemByMinus7(-1));
    expectEquals(0, $noinline$IntRemByMinus7(7));
    expectEquals(0, $noinline$IntRemByMinus7(-7));
    expectEquals(1, $noinline$IntRemByMinus7(22));
    expectEquals(-1, $noinline$IntRemByMinus7(-22));

    expectEquals(0, $noinline$IntRemBy6(0));
    expectEquals(1, $noinline$IntRemBy6(1));
    expectEquals(-1, $noinline$IntRemBy6(-1));
    expectEquals(0, $noinline$IntRemBy6(6));
    expectEquals(0, $noinline$IntRemBy6(-6));
    expectEquals(1, $noinline$IntRemBy6(19));
    expectEquals(-1, $noinline$IntRemBy6(-19));

    expectEquals(0, $noinline$IntALenRemBy6(new int[0]));
    expectEquals(1, $noinline$IntALenRemBy6(new int[1]));
    expectEquals(0, $noinline$IntALenRemBy6(new int[6]));
    expectEquals(1, $noinline$IntALenRemBy6(new int[19]));

    expectEquals(0, $noinline$IntRemByMinus6(0));
    expectEquals(1, $noinline$IntRemByMinus6(1));
    expectEquals(-1, $noinline$IntRemByMinus6(-1));
    expectEquals(0, $noinline$IntRemByMinus6(6));
    expectEquals(0, $noinline$IntRemByMinus6(-6));
    expectEquals(1, $noinline$IntRemByMinus6(19));
    expectEquals(-1, $noinline$IntRemByMinus6(-19));

    expectEquals(1, $noinline$UnsignedIntRem01(13));
    expectEquals(1, $noinline$UnsignedIntRem02(13));
    expectEquals(1, $noinline$UnsignedIntRem03(13));
    expectEquals(1, $noinline$UnsignedIntRem04(13));
    expectEquals(1, $noinline$UnsignedIntRem05(101));
    expectEquals(11, $noinline$UnsignedIntRem06(101));

    expectEquals(-1, $noinline$SignedIntRem01(-13));
    expectEquals(-1, $noinline$SignedIntRem02(-13));
    expectEquals(1, $noinline$SignedIntRem03(-13));
    expectEquals(1, $noinline$SignedIntRem04(-13, true));
    expectEquals(0, $noinline$SignedIntRem05(-12, 0,-13));
    expectEquals(-1, $noinline$SignedIntRem06(-13));
  }

  // A test case to check that 'srai' (shift right arithmetic immediate) is used
  // for division by a constant. For divisor 18, seen in an MP3 decoding workload,
  // there is no need to correct the result of the multiplication by the magic number.
  // Therefore, the compiler can directly use 'srai' to perform the division.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemBy18(int) disassembly (after)
  /// CHECK:                 srai {{\w+}}, {{\w+}}, 34
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemBy18(int v) {
    int r = v % 18;
    return r;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntALenRemBy18(int[]) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntALenRemBy18(int[] arr) {
    int r = arr.length % 18;
    return r;
  }

  // A test case to check that 'srli' and 'srai' are combined into one 'srai'.
  // Divisor -18 has the same property as divisor 18: no need to correct the
  // result of get_high(dividend * magic). So there are no
  // instructions between 'srli' and 'srai'. In such a case they can be combined
  // into one 'srai'.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemByMinus18(int) disassembly (after)
  /// CHECK:                 srai {{\w+}}, {{\w+}}, 34
  /// CHECK:                 srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemByMinus18(int v) {
    int r = v % -18;
    return r;
  }

  // A test case to check that 'srliw' and 'addw' are combined into one 'addw'.
  // For divisor 7 seen in the core library the result of get_high(dividend * magic)
  // must be corrected by the 'addw' instruction.
  //
  // The test case also checks 'addw' and 'add_shift' are optimized into 'addw' and 'srliw'.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemBy7(int) disassembly (after)
  /// CHECK:                 srai {{\w+}}, {{\w+}}, 34
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemBy7(int v) {
    int r = v % 7;
    return r;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$IntALenRemBy7(int[]) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            slli {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntALenRemBy7(int[] arr) {
    int r = arr.length % 7;
    return r;
  }

  // A test case to check that 'lsr' and 'add' are combined into one 'adds'.
  // Divisor -7 has the same property as divisor 7: the result of get_high(dividend * magic)
  // must be corrected. In this case it is a 'sub' instruction.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemByMinus7(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            slli {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 34
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemByMinus7(int v) {
    int r = v % -7;
    return r;
  }

  // A test case to check that 'sraiw' is used to get the high 32 bits of the result of
  // 'dividend * magic'.
  // For divisor 6 seen in the core library there is no need to correct the result of
  // get_high(dividend * magic). Also there is no 'sraiw' before the final 'addw' instruction
  // which uses only the high 32 bits of the result. In such a case 'sraiw' getting the high
  // 32 bits can be used as well.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemBy6(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemBy6(int v) {
    int r = v % 6;
    return r;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$IntALenRemBy6(int[]) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntALenRemBy6(int[] arr) {
    int r = arr.length % 6;
    return r;
  }

  // A test case to check that 'sraiw' is used to get the high 32 bits of the result of
  // 'dividend * magic'.
  // Divisor -6 has the same property as divisor 6: no need to correct the result of
  // get_high(dividend * magic) and no 'sraiw' before the final 'addw' instruction
  // which uses only the high 32 bits of the result. In such a case 'sraiw' getting the high
  // 32 bits can be used as well.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntRemByMinus6(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntRemByMinus6(int v) {
    int r = v % -6;
    return r;
  }

  private static int $noinline$Negate(int v) {
    return -v;
  }

  private static int $noinline$Decrement(int v) {
    return v - 1;
  }

  private static int $noinline$Increment(int v) {
    return v + 1;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem01(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem01(int v) {
    int c = 0;
    if (v > 0) {
      c = Integer.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem02(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem02(int v) {
    int c = 0;
    if (0 < v) {
      c = Integer.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem03(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem03(int v) {
    int c = 0;
    if (v >= 0) {
      c = Integer.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem04(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem04(int v) {
    int c = 0;
    if (0 <= v) {
      c = Integer.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem05(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem05(int v) {
    int c = 0;
    for(; v > 100; ++c) {
      v = Integer.remainderUnsigned(v, 10);
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$UnsignedIntRem06(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRem06(int v) {
    if (v < 10) {
      v = $noinline$Negate(v); // This is to prevent from using Select.
    } else {
      v = Integer.remainderUnsigned(v, 10) + Integer.divideUnsigned(v, 10);
    }
    return v;
  }

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem01(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem01(int v) {
    int c = 0;
    if (v < 0) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    return c;
  }

  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem02(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem02(int v) {
    int c = 0;
    if (v <= 0) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    return c;
  }

  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem03(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem03(int v) {
    boolean positive = (v > 0);
    int c = v % 6;
    if (!positive) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem04(int, boolean) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem04(int v, boolean apply_rem) {
    int c = 0;
    boolean positive = (v > 0);
    if (apply_rem) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    if (!positive) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem05(int, int, int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem05(int v, int a, int b) {
    int c = 0;

    if (v < a)
      c = $noinline$Increment(c); // This is to prevent from using Select.

    if (b < a)
      c = $noinline$Increment(c); // This is to prevent from using Select.

    if (v > b) {
      c = v % 6;
    } else {
      c = $noinline$Increment(c); // This is to prevent from using Select.
    }

    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$SignedIntRem06(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 31
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$SignedIntRem06(int v) {
    int c = v % 6;

    if (v > 0) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }

    return c;
  }

  private static void remLong() {
    expectEquals(0L, $noinline$LongRemBy18(0L));
    expectEquals(1L, $noinline$LongRemBy18(1L));
    expectEquals(-1L, $noinline$LongRemBy18(-1L));
    expectEquals(0L, $noinline$LongRemBy18(18L));
    expectEquals(0L, $noinline$LongRemBy18(-18L));
    expectEquals(11L, $noinline$LongRemBy18(65L));
    expectEquals(-11L, $noinline$LongRemBy18(-65L));

    expectEquals(0L, $noinline$LongRemByMinus18(0L));
    expectEquals(1L, $noinline$LongRemByMinus18(1L));
    expectEquals(-1L, $noinline$LongRemByMinus18(-1L));
    expectEquals(0L, $noinline$LongRemByMinus18(18L));
    expectEquals(0L, $noinline$LongRemByMinus18(-18L));
    expectEquals(11L, $noinline$LongRemByMinus18(65L));
    expectEquals(-11L, $noinline$LongRemByMinus18(-65L));

    expectEquals(0L, $noinline$LongRemBy7(0L));
    expectEquals(1L, $noinline$LongRemBy7(1L));
    expectEquals(-1L, $noinline$LongRemBy7(-1L));
    expectEquals(0L, $noinline$LongRemBy7(7L));
    expectEquals(0L, $noinline$LongRemBy7(-7L));
    expectEquals(1L, $noinline$LongRemBy7(22L));
    expectEquals(-1L, $noinline$LongRemBy7(-22L));

    expectEquals(0L, $noinline$LongRemByMinus7(0L));
    expectEquals(1L, $noinline$LongRemByMinus7(1L));
    expectEquals(-1L, $noinline$LongRemByMinus7(-1L));
    expectEquals(0L, $noinline$LongRemByMinus7(7L));
    expectEquals(0L, $noinline$LongRemByMinus7(-7L));
    expectEquals(1L, $noinline$LongRemByMinus7(22L));
    expectEquals(-1L, $noinline$LongRemByMinus7(-22L));

    expectEquals(0L, $noinline$LongRemBy6(0L));
    expectEquals(1L, $noinline$LongRemBy6(1L));
    expectEquals(-1L, $noinline$LongRemBy6(-1L));
    expectEquals(0L, $noinline$LongRemBy6(6L));
    expectEquals(0L, $noinline$LongRemBy6(-6L));
    expectEquals(1L, $noinline$LongRemBy6(19L));
    expectEquals(-1L, $noinline$LongRemBy6(-19L));

    expectEquals(0L, $noinline$LongRemByMinus6(0L));
    expectEquals(1L, $noinline$LongRemByMinus6(1L));
    expectEquals(-1L, $noinline$LongRemByMinus6(-1L));
    expectEquals(0L, $noinline$LongRemByMinus6(6L));
    expectEquals(0L, $noinline$LongRemByMinus6(-6L));
    expectEquals(1L, $noinline$LongRemByMinus6(19L));
    expectEquals(-1L, $noinline$LongRemByMinus6(-19L));

    expectEquals(0L, $noinline$LongRemBy100(0L));
    expectEquals(1L, $noinline$LongRemBy100(1L));
    expectEquals(-1L, $noinline$LongRemBy100(-1L));
    expectEquals(0L, $noinline$LongRemBy100(100L));
    expectEquals(0L, $noinline$LongRemBy100(-100L));
    expectEquals(1L, $noinline$LongRemBy100(101L));
    expectEquals(-1L, $noinline$LongRemBy100(-101L));

    expectEquals(0L, $noinline$LongRemByMinus100(0L));
    expectEquals(1L, $noinline$LongRemByMinus100(1L));
    expectEquals(-1L, $noinline$LongRemByMinus100(-1L));
    expectEquals(0L, $noinline$LongRemByMinus100(100L));
    expectEquals(0L, $noinline$LongRemByMinus100(-100L));
    expectEquals(1L, $noinline$LongRemByMinus100(101L));
    expectEquals(-1L, $noinline$LongRemByMinus100(-101L));

    expectEquals(1L, $noinline$UnsignedLongRem01(13L));
    expectEquals(1L, $noinline$UnsignedLongRem02(13L));
    expectEquals(1L, $noinline$UnsignedLongRem03(13L));
    expectEquals(1L, $noinline$UnsignedLongRem04(13L));
    expectEquals(1L, $noinline$UnsignedLongRem05(101L));
    expectEquals(11L, $noinline$UnsignedLongRem06(101L));

    expectEquals(-1L, $noinline$SignedLongRem01(-13L));
    expectEquals(-1L, $noinline$SignedLongRem02(-13L));
    expectEquals(1L, $noinline$SignedLongRem03(-13L));
    expectEquals(1L, $noinline$SignedLongRem04(-13L, true));
    expectEquals(0L, $noinline$SignedLongRem05(-12L, 0L,-13L));
    expectEquals(-1L, $noinline$SignedLongRem06(-13L));
  }

  // Test cases for Int64 HDiv/HRem to check that optimizations implemented for Int32 are not
  // used for Int64. The same divisors 18, -18, 7, -7, 6 and -6 are used.

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemBy18(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemBy18(long v) {
    long r = v % 18L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemByMinus18(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemByMinus18(long v) {
    long r = v % -18L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemBy7(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 1
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemBy7(long v) {
    long r = v % 7L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemByMinus7(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 1
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemByMinus7(long v) {
    long r = v % -7L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemBy6(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemBy6(long v) {
    long r = v % 6L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemByMinus6(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemByMinus6(long v) {
    long r = v % -6L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemBy100(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 6
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemBy100(long v) {
    long r = v % 100L;
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongRemByMinus100(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srai {{\w+}}, {{\w+}}, 6
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongRemByMinus100(long v) {
    long r = v % -100L;
    return r;
  }

  private static long $noinline$Negate(long v) {
    return -v;
  }

  private static long $noinline$Decrement(long v) {
    return v - 1;
  }

  private static long $noinline$Increment(long v) {
    return v + 1;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem01(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem01(long v) {
    long c = 0;
    if (v > 0) {
      c = Long.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem02(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem02(long v) {
    long c = 0;
    if (0 < v) {
      c = Long.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem03(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem03(long v) {
    long c = 0;
    if (v >= 0) {
      c = Long.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem04(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem04(long v) {
    long c = 0;
    if (0 <= v) {
      c = Long.remainderUnsigned(v, 6);
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem05(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 2
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem05(long v) {
    long c = 0;
    for(; v > 100; ++c) {
      v = Long.remainderUnsigned(v, 10);
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$UnsignedLongRem06(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 2
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$UnsignedLongRem06(long v) {
    if (v < 10) {
      v = $noinline$Negate(v); // This is to prevent from using Select.
    } else {
      v = Long.remainderUnsigned(v, 10) + Long.divideUnsigned(v, 10);
    }
    return v;
  }

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem01(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem01(long v) {
    long c = 0;
    if (v < 0) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem02(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem02(long v) {
    long c = 0;
    if (v <= 0) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem03(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem03(long v) {
    boolean positive = (v > 0);
    long c = v % 6;
    if (!positive) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem04(long, boolean) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem04(long v, boolean apply_rem) {
    long c = 0;
    boolean positive = (v > 0);
    if (apply_rem) {
      c = v % 6;
    } else {
      c = $noinline$Decrement(v); // This is to prevent from using Select.
    }
    if (!positive) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem05(long, long, long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem05(long v, long a, long b) {
    long c = 0;

    if (v < a)
      c = $noinline$Increment(c); // This is to prevent from using Select.

    if (b < a)
      c = $noinline$Increment(c); // This is to prevent from using Select.

    if (v > b) {
      c = v % 6;
    } else {
      c = $noinline$Increment(c); // This is to prevent from using Select.
    }

    return c;
  }

  // A test case to check that a correcting 'add' is generated for signed division.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$SignedLongRem06(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$SignedLongRem06(long v) {
    long c = v % 6;

    if (v > 0) {
      c = $noinline$Negate(c); // This is to prevent from using Select.
    }

    return c;
  }
}
