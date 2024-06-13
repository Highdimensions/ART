/*
 * Copyright (C) 2017 The Android Open Source Project
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
  static byte static_byte;
  static char static_char;
  static int static_int;
  static int unrelated_static_int;

  public static void assertByteEquals(byte expected, byte result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertShortEquals(short expected, short result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertCharEquals(char expected, char result) {
    if (expected != result) {
      // Values are cast to int to display numeric values instead of
      // (UTF-16 encoded) characters.
      throw new Error("Expected: " + (int)expected + ", found: " + (int)result);
    }
  }

  /// CHECK-START: byte Main.getByte1() constant_folding (before)
  /// CHECK: TypeConversion
  /// CHECK: TypeConversion
  /// CHECK: Add
  /// CHECK: TypeConversion

  /// CHECK-START: byte Main.getByte1() constant_folding (after)
  /// CHECK-NOT: TypeConversion
  /// CHECK-NOT: Add

  static byte getByte1() {
    int i = -2;
    int j = -3;
    return (byte)((byte)i + (byte)j);
  }

  /// CHECK-START: byte Main.getByte2() constant_folding (before)
  /// CHECK: TypeConversion
  /// CHECK: TypeConversion
  /// CHECK: Add
  /// CHECK: TypeConversion

  /// CHECK-START: byte Main.getByte2() constant_folding (after)
  /// CHECK-NOT: TypeConversion
  /// CHECK-NOT: Add

  static byte getByte2() {
    int i = -100;
    int j = -101;
    return (byte)((byte)i + (byte)j);
  }

  /// CHECK-START: byte Main.getByte3() constant_folding (before)
  /// CHECK: TypeConversion
  /// CHECK: TypeConversion
  /// CHECK: Add
  /// CHECK: TypeConversion

  /// CHECK-START: byte Main.getByte2() constant_folding (after)
  /// CHECK-NOT: TypeConversion
  /// CHECK-NOT: Add

  static byte getByte3() {
    long i = 0xabcdabcdabcdL;
    return (byte)((byte)i + (byte)i);
  }

  static byte byteVal = -1;
  static short shortVal = -1;
  static char charVal = 0xffff;
  static int intVal = -1;

  static byte[] byteArr = { 0 };
  static short[] shortArr = { 0 };
  static char[] charArr = { 0 };
  static int[] intArr = { 0 };

  static byte $noinline$getByte() {
    return byteVal;
  }

  static short $noinline$getShort() {
    return shortVal;
  }

  static char $noinline$getChar() {
    return charVal;
  }

  static int $noinline$getInt() {
    return intVal;
  }

  static boolean sFlag = true;

  /// CHECK-START: void Main.byteToShort() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void byteToShort() {
    shortArr[0] = 0;
    if (sFlag) {
      shortArr[0] = $noinline$getByte();
    }
  }

  /// CHECK-START: void Main.byteToChar() instruction_simplifier$before_codegen (after)
  /// CHECK: TypeConversion
  private static void byteToChar() {
    charArr[0] = 0;
    if (sFlag) {
      charArr[0] = (char)$noinline$getByte();
    }
  }

  /// CHECK-START: void Main.byteToInt() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void byteToInt() {
    intArr[0] = 0;
    if (sFlag) {
      intArr[0] = $noinline$getByte();
    }
  }

  /// CHECK-START: void Main.charToByte() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void charToByte() {
    byteArr[0] = 0;
    if (sFlag) {
      byteArr[0] = (byte)$noinline$getChar();
    }
  }

  /// CHECK-START: void Main.charToShort() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void charToShort() {
    shortArr[0] = 0;
    if (sFlag) {
      shortArr[0] = (short)$noinline$getChar();
    }
  }

  /// CHECK-START: void Main.charToInt() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void charToInt() {
    intArr[0] = 0;
    if (sFlag) {
      intArr[0] = $noinline$getChar();
    }
  }

  /// CHECK-START: void Main.shortToByte() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void shortToByte() {
    byteArr[0] = 0;
    if (sFlag) {
      byteArr[0] = (byte)$noinline$getShort();
    }
  }

  /// CHECK-START: void Main.shortToChar() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void shortToChar() {
    charArr[0] = 0;
    if (sFlag) {
      charArr[0] = (char)$noinline$getShort();
    }
  }

  /// CHECK-START: void Main.shortToInt() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void shortToInt() {
    intArr[0] = 0;
    if (sFlag) {
      intArr[0] = $noinline$getShort();
    }
  }

  /// CHECK-START: void Main.intToByte() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void intToByte() {
    byteArr[0] = 0;
    if (sFlag) {
      byteArr[0] = (byte)$noinline$getInt();
    }
  }

  /// CHECK-START: void Main.intToShort() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void intToShort() {
    shortArr[0] = 0;
    if (sFlag) {
      shortArr[0] = (short)$noinline$getInt();
    }
  }

  /// CHECK-START: void Main.intToChar() instruction_simplifier$before_codegen (after)
  /// CHECK-NOT: TypeConversion
  private static void intToChar() {
    charArr[0] = 0;
    if (sFlag) {
      charArr[0] = (char)$noinline$getInt();
    }
  }

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionInt8(int) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_byte
  /// CHECK-DAG:                StaticFieldSet field_name:Main.unrelated_static_int loop:B{{\d+}}
  /// CHECK-DAG: <<GetB:b\d+>>  StaticFieldGet field_name:Main.static_byte
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetB>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI:i\d+>>  StaticFieldGet field_name:Main.static_int field_type:Int32
  /// CHECK-DAG:                Return [<<GetI>>]

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionInt8(int) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<Conv:b\d+>>  TypeConversion [<<Value>>]
  /// CHECK-DAG:                Return [<<Conv>>]
  public static int $noinline$loopPhiStoreLoadConversionInt8(int value) {
    // -120 - 24 is equal to 112 in `byte` due to wraparounds.
    static_byte = (byte) value;
    // Irrelevant code but needed to make LSE use loop Phi placeholders.
    for (int q = 1; q < 12; q++) {
      unrelated_static_int = 24;
    }
    static_int = static_byte;
    return static_int;
  }

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionUint8(int) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_byte
  /// CHECK-DAG:                StaticFieldSet field_name:Main.unrelated_static_int loop:B{{\d+}}
  // The `& 0xff` has already been merged with the load.
  /// CHECK-DAG: <<GetA:a\d+>>  StaticFieldGet field_name:Main.static_byte
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetA>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI:i\d+>>  StaticFieldGet field_name:Main.static_int field_type:Int32
  /// CHECK-DAG:                Return [<<GetI>>]

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionUint8(int) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<Conv:a\d+>>  TypeConversion [<<Value>>]
  /// CHECK-DAG:                Return [<<Conv>>]
  public static int $noinline$loopPhiStoreLoadConversionUint8(int value) {
    // -120 - 24 is equal to 112 in `byte` due to wraparounds.
    static_byte = (byte) value;
    // Irrelevant code but needed to make LSE use loop Phi placeholders.
    for (int q = 1; q < 12; q++) {
        unrelated_static_int = 24;
    }
    static_int = static_byte & 0xff;
    return static_int;
  }

  public static int $noinline$loopPhiTwoStoreLoadConversions(int value) {
      static_byte = (byte) value;
      // Irrelevant code but needed to make LSE use loop Phi placeholders.
      for (int q = 1; q < 12; q++) {
          static_int = 24;
      }
      static_int = static_byte;
      static_char = (char) static_int;
      static_int = static_char;
      return static_int;
  }

  public static int $noinline$conditionalStoreLoadConversionInt8InLoop(int value, boolean cond) {
      static_int = value;
      for (int q = 1; q < 12; q++) {
          if (cond) {
              static_byte = (byte) static_int;
              static_int = static_byte;
          }
      }
      return static_int;
  }

  public static int $noinline$conditionalStoreLoadConversionUint8InLoop(int value, boolean cond) {
      static_int = value;
      for (int q = 1; q < 12; q++) {
          if (cond) {
              static_byte = (byte) static_int;
              static_int = static_byte & 0xff;
          }
      }
      return static_int;
  }

  public static int $noinline$twoConditionalStoreLoadConversionsInLoop(int value, boolean cond) {
      static_int = value;
      for (int q = 1; q < 12; q++) {
          if (cond) {
              static_byte = (byte) static_int;
              static_char = (char) static_byte;
              static_int = static_char;
          }
      }
      return static_int;
  }

  public static void main(String[] args) {
    assertByteEquals(getByte1(), (byte)-5);
    assertByteEquals(getByte2(), (byte)(-201));
    assertByteEquals(getByte3(), (byte)(0xcd + 0xcd));

    byteToShort();
    assertShortEquals(shortArr[0], (short)-1);
    byteToChar();
    assertCharEquals(charArr[0], (char)-1);
    byteToInt();
    assertIntEquals(intArr[0], -1);
    charToByte();
    assertByteEquals(byteArr[0], (byte)-1);
    charToShort();
    assertShortEquals(shortArr[0], (short)-1);
    charToInt();
    assertIntEquals(intArr[0], 0xffff);
    shortToByte();
    assertByteEquals(byteArr[0], (byte)-1);
    shortToChar();
    assertCharEquals(charArr[0], (char)-1);
    shortToInt();
    assertIntEquals(intArr[0], -1);
    intToByte();
    assertByteEquals(byteArr[0], (byte)-1);
    intToShort();
    assertShortEquals(shortArr[0], (short)-1);
    intToChar();
    assertCharEquals(charArr[0], (char)-1);

    System.out.println($noinline$loopPhiStoreLoadConversionInt8(42));  // 42
    System.out.println($noinline$loopPhiStoreLoadConversionInt8(-42));  // -42
    System.out.println($noinline$loopPhiStoreLoadConversionInt8(128));  // -128
    System.out.println($noinline$loopPhiStoreLoadConversionInt8(-129));  // 127

    System.out.println($noinline$loopPhiStoreLoadConversionUint8(42));  // 42
    System.out.println($noinline$loopPhiStoreLoadConversionUint8(-42));  // 214
    System.out.println($noinline$loopPhiStoreLoadConversionUint8(128));  // 128
    System.out.println($noinline$loopPhiStoreLoadConversionUint8(-129));  // 127

    System.out.println($noinline$loopPhiTwoStoreLoadConversions(42));  // 42
    System.out.println($noinline$loopPhiTwoStoreLoadConversions(-42));  // 65494
    System.out.println($noinline$loopPhiTwoStoreLoadConversions(128));  // 65408
    System.out.println($noinline$loopPhiTwoStoreLoadConversions(-129));  // 127
    System.out.println($noinline$loopPhiTwoStoreLoadConversions(256));  // 0
    System.out.println($noinline$loopPhiTwoStoreLoadConversions(-257));  // 65535

    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(42, false));  // 42
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(42, true));  // 42
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(-42, false));  // -42
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(-42, true));  // -42
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(128, false));  // 128
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(128, true));  // -128
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(-129, false));  // -129
    System.out.println($noinline$conditionalStoreLoadConversionInt8InLoop(-129, true));  // 127

    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(42, false));  // 42
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(42, true));  // 42
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(-42, false));  // -42
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(-42, true));  // 214
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(128, false));  // 128
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(128, true));  // -128
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(-129, false));  // -129
    System.out.println($noinline$conditionalStoreLoadConversionUint8InLoop(-129, true));  // 127

    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(42, false));  // 42
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(42, true));  // 42
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-42, false));  // -42
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-42, true));  // 65494
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(128, false));  // 128
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(128, true));  // 65408
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-129, false));  // -129
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-129, true));  // 127
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(256, false));  // 256
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(256, true));  // 0
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-257, false));  // -257
    System.out.println($noinline$twoConditionalStoreLoadConversionsInLoop(-257, true));  // 65535
  }
}
