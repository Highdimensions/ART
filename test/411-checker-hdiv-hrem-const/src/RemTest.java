/*
 * Copyright (C) 2020 The Android Open Source Project
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

    expectEquals(1, $noinline$PositiveIntRem01(13));
    expectEquals(1, $noinline$PositiveIntRem02(13));
    expectEquals(1, $noinline$PositiveIntRem03(13));
    expectEquals(1, $noinline$PositiveIntRem04(13));
    expectEquals(1, $noinline$PositiveIntRem05(101));
    expectEquals(11, $noinline$PositiveIntRem06(101));

    expectEquals(-1, $noinline$SignedIntRem01(-13));
    expectEquals(-1, $noinline$SignedIntRem02(-13));
    expectEquals(1, $noinline$SignedIntRem03(-13));
    expectEquals(1, $noinline$SignedIntRem04(-13, true));
    expectEquals(0, $noinline$SignedIntRem05(-12, 0,-13));
    expectEquals(-1, $noinline$SignedIntRem06(-13));

    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary1(0));
    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary1(Integer.parseUnsignedInt("4294967295")));
    expectEquals(1, $noinline$IntUnsignedRemNearOverflowBoundary1(1));
    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary2(0));
    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary2(Integer.parseUnsignedInt("2147483648")));
    expectEquals(1, $noinline$IntUnsignedRemNearOverflowBoundary2(1));
    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary3(0));
    expectEquals(0, $noinline$IntUnsignedRemNearOverflowBoundary3(Integer.parseUnsignedInt("3221225471")));
    expectEquals(1, $noinline$IntUnsignedRemNearOverflowBoundary3(1));

    expectEquals(0, $noinline$UnsignedIntRemBy3(0));
    expectEquals(1, $noinline$UnsignedIntRemBy3(1));
    expectEquals(0, $noinline$UnsignedIntRemBy3(3));
    expectEquals(1, $noinline$UnsignedIntRemBy3(10));
    expectEquals(0, $noinline$UnsignedIntRemBy3(Integer.parseUnsignedInt("4294967295")));
    expectEquals(2, $noinline$UnsignedIntRemBy3(Integer.parseUnsignedInt("2147483648")));
    expectEquals(2, $noinline$UnsignedIntRemBy3(Integer.parseUnsignedInt("3221225471")));

    expectEquals(0, $noinline$UnsignedIntRemBy7(0));
    expectEquals(1, $noinline$UnsignedIntRemBy7(1));
    expectEquals(0, $noinline$UnsignedIntRemBy7(7));
    expectEquals(3, $noinline$UnsignedIntRemBy7(10));
    expectEquals(3, $noinline$UnsignedIntRemBy7(Integer.parseUnsignedInt("4294967295")));
    expectEquals(2, $noinline$UnsignedIntRemBy7(Integer.parseUnsignedInt("2147483648")));
    expectEquals(2, $noinline$UnsignedIntRemBy7(Integer.parseUnsignedInt("3221225471")));

    expectEquals(0, $noinline$UnsignedIntRemBy1(0));
    expectEquals(0, $noinline$UnsignedIntRemBy1(1));
    expectEquals(0, $noinline$UnsignedIntRemBy1(10));
    expectEquals(0, $noinline$UnsignedIntRemBy1(42));
    expectEquals(0, $noinline$UnsignedIntRemBy1(Integer.parseUnsignedInt("4294967295")));
    expectEquals(0, $noinline$UnsignedIntRemBy1(Integer.parseUnsignedInt("2147483648")));
    expectEquals(0, $noinline$UnsignedIntRemBy1(Integer.parseUnsignedInt("3221225471")));
  }

  // A test case to check that shift operations are optimized for remainder calculation
  // with divisor 18. For divisor 18 seen in MP3 decoding workloads, there is no need
  // to correct the result of high part multiplication by magic number, so shift
  // operations can be combined for efficient remainder computation.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemBy18(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  /// CHECK-NEXT:            mov w{{\d+}}, #0x12
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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
  /// CHECK-START-ARM:   int RemTest.$noinline$IntALenRemBy18(int[]) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            lsr{{s?}} r{{\d+}}, #2
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #18
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntALenRemBy18(int[]) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            mov w{{\d+}}, #0x12
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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

  // A test case to check that shift operations are optimized for remainder calculation
  // with divisor -18. Divisor -18 has the same property as divisor 18: no need to correct
  // the result of high part multiplication by magic number, so shift operations can be
  // combined for efficient remainder computation.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemByMinus18(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  /// CHECK-NEXT:            mov w{{\d+}}, #0xffffffee
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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

  // A test case to check that shift and add operations are optimized for remainder
  // calculation with divisor 7. For divisor 7 seen in the core library, the result
  // of high part multiplication by magic number must be corrected by addition.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemBy7(int) disassembly (after)
  /// CHECK:                 adds x{{\d+}}, x{{\d+}}, x{{\d+}}, lsl #32
  /// CHECK-NEXT:            asr  x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            cinc w{{\d+}}, w{{\d+}}, mi
  /// CHECK-NEXT:            mov w{{\d+}}, #0x7
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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
  /// CHECK-START-ARM:   int RemTest.$noinline$IntALenRemBy7(int[]) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            add{{s?}} r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            lsr{{s?}} r{{\d+}}, #2
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #7
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntALenRemBy7(int[]) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            mov w{{\d+}}, #0x7
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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

  // A test case to check that shift and subtract operations are optimized for remainder
  // calculation with divisor -7. Divisor -7 has the same property as divisor 7: the result
  // of high part multiplication by magic number must be corrected, but with subtraction
  // instead of addition.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemByMinus7(int) disassembly (after)
  /// CHECK:                 subs x{{\d+}}, x{{\d+}}, x{{\d+}}, lsl #32
  /// CHECK-NEXT:            asr  x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            cinc w{{\d+}}, w{{\d+}}, mi
  /// CHECK-NEXT:            mov w{{\d+}}, #0xfffffff9
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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

  // A test case to check that arithmetic shift right is used to get the high 32 bits
  // of the multiplication result for remainder calculation. For divisor 6 seen in the
  // core library, there is no need to correct the result of high part multiplication
  // by magic number.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemBy6(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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
  /// CHECK-START-ARM:   int RemTest.$noinline$IntALenRemBy6(int[]) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntALenRemBy6(int[]) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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

  // A test case to check that arithmetic shift right is used to get the high 32 bits
  // of the multiplication result for remainder calculation. Divisor -6 has the same
  // property as divisor 6: no need to correct the result of high part multiplication
  // by magic number.
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$IntRemByMinus6(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  /// CHECK-NEXT:            mov w{{\d+}}, #0xfffffffa
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
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
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem01(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem01(int) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem01(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem01(int v) {
    int c = 0;
    if (v > 0) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem02(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem02(int) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem02(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem02(int v) {
    int c = 0;
    if (0 < v) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem03(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem03(int) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem03(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem03(int v) {
    int c = 0;
    if (v >= 0) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem04(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem04(int) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            mov w{{\d+}}, #0x6
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem04(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem04(int v) {
    int c = 0;
    if (0 <= v) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem05(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            lsr{{s?}} r{{\d+}}, #2
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #10
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem05(int) disassembly (after)
  /// CHECK:                 lsr x{{\d+}}, x{{\d+}}, #34
  /// CHECK-NEXT:            mov w{{\d+}}, #0xa
  /// CHECK-NEXT:            msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem05(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem05(int v) {
    int c = 0;
    for(; v > 100; ++c) {
      v %= 10;
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$PositiveIntRem06(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            lsr{{s?}} r{{\d+}}, r{{\d+}}, #2
  /// CHECK:                 mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$PositiveIntRem06(int) disassembly (after)
  /// CHECK:                 smull x{{\d+}}, w{{\d+}}, w{{\d+}}
  /// CHECK-NEXT:            lsr x{{\d+}}, x{{\d+}}, #34
  /// CHECK:                 msub w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}
  //
  /// CHECK-START-RISCV64:   int RemTest.$noinline$PositiveIntRem06(int) disassembly (after)
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 34
  /// CHECK:                 mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$PositiveIntRem06(int v) {
    if (v < 10) {
      v = $noinline$Negate(v); // This is to prevent from using Select.
    } else {
      v = (v % 10) + (v / 10);
    }
    return v;
  }

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem01(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem01(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
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

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem02(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem02(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  //
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem03(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem03(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
  //
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem04(int, boolean) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem04(int, boolean) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem05(int, int, int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem05(int, int, int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM:   int RemTest.$noinline$SignedIntRem06(int) disassembly (after)
  /// CHECK:                 smull     r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  /// CHECK-NEXT:            sub       r{{\d+}}, r{{\d+}}, asr #31
  /// CHECK-NEXT:            mov{{s?}} r{{\d+}}, #6
  /// CHECK-NEXT:            mls       r{{\d+}}, r{{\d+}}, r{{\d+}}, r{{\d+}}
  //
  /// CHECK-START-ARM64: int RemTest.$noinline$SignedIntRem06(int) disassembly (after)
  /// CHECK:                 asr x{{\d+}}, x{{\d+}}, #32
  /// CHECK-NEXT:            add w{{\d+}}, w{{\d+}}, w{{\d+}}, lsr #31
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

  //The next three tests check that division with remainder works
  //correctly for divisors with the high bit set.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$IntUnsignedRemNearOverflowBoundary1(int) disassembly (after)
  /// CHECK:                 c.slli {{\w+}}, 32
  /// CHECK-NEXT:            c.srli {{\w+}}, 32
  /// CHECK-NEXT:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK:            mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntUnsignedRemNearOverflowBoundary1(int v) {
    int r = Integer.remainderUnsigned(v, 0xFFFFFFFF);
    return r;
  }

  /// CHECK-START-RISCV64: int RemTest.$noinline$IntUnsignedRemNearOverflowBoundary2(int) disassembly (after)
  /// CHECK:                 c.slli {{\w+}}, 32
  /// CHECK-NEXT:            c.srli {{\w+}}, 32
  /// CHECK-NEXT:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK:            mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntUnsignedRemNearOverflowBoundary2(int v) {
    int r = Integer.remainderUnsigned(v, 0x80000000);
    return r;
  }

  /// CHECK-START-RISCV64: int RemTest.$noinline$IntUnsignedRemNearOverflowBoundary3(int) disassembly (after)
  /// CHECK:                 c.slli {{\w+}}, 32
  /// CHECK-NEXT:            c.srli {{\w+}}, 32
  /// CHECK-NEXT:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 61
  /// CHECK:            mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$IntUnsignedRemNearOverflowBoundary3(int v) {
    int r = Integer.remainderUnsigned(v, 0xBFFFFFFF);
    return r;
  }

  //This test checks the correctness of division with remainder for a
  //divisor that does not require correction by addition.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$UnsignedIntRemBy3(int) disassembly (after)
  /// CHECK:                 c.slli {{\w+}}, 32
  /// CHECK-NEXT:            c.srli {{\w+}}, 32
  /// CHECK-NEXT:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 33
  /// CHECK:            mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRemBy3(int v) {
    int r = Integer.remainderUnsigned(v, 3);
    return r;
  }

  //This test checks the correctness of division with remainder for a
  //divisor that require correction by addition.
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$UnsignedIntRemBy7(int) disassembly (after)
  /// CHECK:                 c.slli {{\w+}}, 32
  /// CHECK-NEXT:            c.srli {{\w+}}, 32
  /// CHECK-NEXT:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 32
  /// CHECK-NEXT:            subw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 1
  /// CHECK-NEXT:            addw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srliw {{\w+}}, {{\w+}}, 2
  /// CHECK:            mulw {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            subw {{\w+}}, {{\w+}}, {{\w+}}
  private static int $noinline$UnsignedIntRemBy7(int v) {
    int r = Integer.remainderUnsigned(v, 7);
    return r;
  }

  //This test checks the correctness of division with remainder by 1
  //
  /// CHECK-START-RISCV64: int RemTest.$noinline$UnsignedIntRemBy1(int) disassembly (after)
  /// CHECK:                 c.li {{\w+}}, 0
  private static int $noinline$UnsignedIntRemBy1(int v) {
    int r = Integer.remainderUnsigned(v, 1);
    return r;
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

    expectEquals(1L, $noinline$PositiveLongRem01(13L));
    expectEquals(1L, $noinline$PositiveLongRem02(13L));
    expectEquals(1L, $noinline$PositiveLongRem03(13L));
    expectEquals(1L, $noinline$PositiveLongRem04(13L));
    expectEquals(1L, $noinline$PositiveLongRem05(101L));
    expectEquals(11L, $noinline$PositiveLongRem06(101L));

    expectEquals(-1L, $noinline$SignedLongRem01(-13L));
    expectEquals(-1L, $noinline$SignedLongRem02(-13L));
    expectEquals(1L, $noinline$SignedLongRem03(-13L));
    expectEquals(1L, $noinline$SignedLongRem04(-13L, true));
    expectEquals(0L, $noinline$SignedLongRem05(-12L, 0L,-13L));
    expectEquals(-1L, $noinline$SignedLongRem06(-13L));

    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary1(0L));
    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary1(Long.parseUnsignedLong("18446744073709551615")));
    expectEquals(1L, $noinline$LongUnsignedRemNearOverflowBoundary1(1L));
    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary2(0L));
    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary2(Long.parseUnsignedLong("9223372036854775808")));
    expectEquals(1L, $noinline$LongUnsignedRemNearOverflowBoundary2(1L));
    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary3(0L));
    expectEquals(0L, $noinline$LongUnsignedRemNearOverflowBoundary3(Long.parseUnsignedLong("13835058055282163711")));
    expectEquals(1L, $noinline$LongUnsignedRemNearOverflowBoundary3(1L));

    expectEquals(0L, $noinline$LongUnsignedRemBy3(0L));
    expectEquals(1L, $noinline$LongUnsignedRemBy3(1L));
    expectEquals(0L, $noinline$LongUnsignedRemBy3(3L));
    expectEquals(1L, $noinline$LongUnsignedRemBy3(10L));
    expectEquals(0L, $noinline$LongUnsignedRemBy3(Long.parseUnsignedLong("18446744073709551615")));
    expectEquals(2L, $noinline$LongUnsignedRemBy3(Long.parseUnsignedLong("9223372036854775808")));
    expectEquals(2L, $noinline$LongUnsignedRemBy3(Long.parseUnsignedLong("13835058055282163711")));

    expectEquals(0L, $noinline$LongUnsignedRemBy7(0L));
    expectEquals(1L, $noinline$LongUnsignedRemBy7(1L));
    expectEquals(0L, $noinline$LongUnsignedRemBy7(7L));
    expectEquals(3L, $noinline$LongUnsignedRemBy7(10L));
    expectEquals(1L, $noinline$LongUnsignedRemBy7(Long.parseUnsignedLong("18446744073709551615")));
    expectEquals(1L, $noinline$LongUnsignedRemBy7(Long.parseUnsignedLong("9223372036854775808")));
    expectEquals(4L, $noinline$LongUnsignedRemBy7(Long.parseUnsignedLong("13835058055282163711")));

    expectEquals(0L, $noinline$LongUnsignedRemBy1(0L));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(1L));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(10L));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(42L));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(Long.parseUnsignedLong("18446744073709551615")));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(Long.parseUnsignedLong("9223372036854775808")));
    expectEquals(0L, $noinline$LongUnsignedRemBy1(Long.parseUnsignedLong("13835058055282163711")));
  }

  // Test cases for Int64 remainder calculations to check that optimizations implemented
  // for Int32 are not used for Int64. The same divisors 18, -18, 7, -7, 6 and -6 are used.

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemBy18(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x12
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemByMinus18(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0xffffffffffffffee
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemBy7(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            asr x{{\d+}}, x{{\d+}}, #1
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x7
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemByMinus7(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            asr x{{\d+}}, x{{\d+}}, #1
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0xfffffffffffffff9
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemBy6(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemByMinus6(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0xfffffffffffffffa
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  // A test to check that add and add_shift operations are optimized for remainder
  // calculation with divisor 100.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemBy100(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            adds  x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            asr   x{{\d+}}, x{{\d+}}, #6
  /// CHECK-NEXT:            cinc  x{{\d+}}, x{{\d+}}, mi
  /// CHECK-NEXT:            mov x{{\d+}}, #0x64
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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

  // A test to check that subtract and add_shift operations are optimized for remainder
  // calculation with divisor -100.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$LongRemByMinus100(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            subs  x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            asr   x{{\d+}}, x{{\d+}}, #6
  /// CHECK-NEXT:            cinc  x{{\d+}}, x{{\d+}}, mi
  /// CHECK-NEXT:            mov x{{\d+}}, #0xffffffffffffff9c
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
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
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem01(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem01(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem01(long v) {
    long c = 0;
    if (v > 0) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem02(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem02(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem02(long v) {
    long c = 0;
    if (0 < v) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem03(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem03(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem03(long v) {
    long c = 0;
    if (v >= 0) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem04(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem04(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem04(long v) {
    long c = 0;
    if (0 <= v) {
      c = v % 6;
    } else {
      c = $noinline$Negate(v); // This is to prevent from using Select.
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem05(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            lsr x{{\d+}}, x{{\d+}}, #2
  /// CHECK-NEXT:            mov x{{\d+}}, #0xa
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem05(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 2
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem05(long v) {
    long c = 0;
    for(; v > 100; ++c) {
      v %= 10;
    }
    return c;
  }

  // A test case to check that a correcting 'add' is not generated for a non-negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$PositiveLongRem06(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            lsr x{{\d+}}, x{{\d+}}, #2
  /// CHECK:                 msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$PositiveLongRem06(long) disassembly (after)
  /// CHECK:                 mulh {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 2
  /// CHECK:                 mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$PositiveLongRem06(long v) {
    if (v < 10) {
      v = $noinline$Negate(v); // This is to prevent from using Select.
    } else {
      v = (v % 10) + (v / 10);
    }
    return v;
  }

  // A test case to check that a correcting 'add' is generated for a negative
  // dividend and a positive divisor.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem01(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem02(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem03(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem04(long, boolean) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem05(long, long, long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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

  // A test case to check that a correcting 'add' is generated for signed remainder calculation.
  //
  /// CHECK-START-ARM64: long RemTest.$noinline$SignedLongRem06(long) disassembly (after)
  /// CHECK:                 smulh x{{\d+}}, x{{\d+}}, x{{\d+}}
  /// CHECK-NEXT:            add x{{\d+}}, x{{\d+}}, x{{\d+}}, lsr #63
  /// CHECK-NEXT:            mov x{{\d+}}, #0x6
  /// CHECK-NEXT:            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}
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

  //The next three tests check that division with remainder works
  //correctly for divisors with the high bit set.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemNearOverflowBoundary1(long) disassembly (after)
  /// CHECK:                 mulhu {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 63
  /// CHECK:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongUnsignedRemNearOverflowBoundary1(long v) {
    long r = Long.remainderUnsigned(v, 0xFFFFFFFFFFFFFFFFL);
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemNearOverflowBoundary2(long) disassembly (after)
  /// CHECK:                 mulhu {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongUnsignedRemNearOverflowBoundary2(long v) {
    long r = Long.remainderUnsigned(v, 0x8000000000000000L);
    return r;
  }

  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemNearOverflowBoundary3(long) disassembly (after)
  /// CHECK:                 mulhu {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 61
  /// CHECK:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongUnsignedRemNearOverflowBoundary3(long v) {
    long r = Long.remainderUnsigned(v, 0xBFFFFFFFFFFFFFFFL);
    return r;
  }

  //This test checks the correctness of division with remainder for a
  //divisor that does not require correction by addition.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemBy3(long) disassembly (after)
  /// CHECK:                 mulhu {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            srli {{\w+}}, {{\w+}}, 1
  /// CHECK:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongUnsignedRemBy3(long v) {
    long r = Long.remainderUnsigned(v, 3L);
    return r;
  }

  //This test checks the correctness of division with remainder for a
  //divisor that require correction by addition.
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemBy7(long) disassembly (after)
  /// CHECK:                 mulhu {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            sub {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            c.srli {{\w+}}, 1
  /// CHECK-NEXT:            c.add {{\w+}}, {{\w+}}
  /// CHECK-NEXT:            c.srli {{\w+}}, 2
  /// CHECK:            mul {{\w+}}, {{\w+}}, {{\w+}}
  /// CHECK:            sub {{\w+}}, {{\w+}}, {{\w+}}
  private static long $noinline$LongUnsignedRemBy7(long v) {
    long r = Long.remainderUnsigned(v, 7L);
    return r;
  }

  //This test checks the correctness of division with remainder by 1
  //
  /// CHECK-START-RISCV64: long RemTest.$noinline$LongUnsignedRemBy1(long) disassembly (after)
  /// CHECK:                 c.li {{\w+}}, 0
  private static long $noinline$LongUnsignedRemBy1(long v) {
    long r = Long.remainderUnsigned(v, 1L);
    return r;
  }
}
