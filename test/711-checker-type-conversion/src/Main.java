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

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionInt8(int) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
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

  /// CHECK-START: int Main.$noinline$loopPhiStoreLoadConversionUint8(int) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
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

  /// CHECK-START: int Main.$noinline$loopPhiTwoStoreLoadConversions(int) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_byte
  /// CHECK-DAG:                StaticFieldSet field_name:Main.unrelated_static_int loop:B{{\d+}}
  /// CHECK-DAG: <<GetB:b\d+>>  StaticFieldGet field_name:Main.static_byte
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetB>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI1:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetI1>>] field_name:Main.static_char
  /// CHECK-DAG: <<GetC:c\d+>>  StaticFieldGet field_name:Main.static_char field_type:Uint16
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetC>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI2:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32
  /// CHECK-DAG:                Return [<<GetI2>>]

  /// CHECK-START: int Main.$noinline$loopPhiTwoStoreLoadConversions(int) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<ConvB:b\d+>> TypeConversion [<<Value>>]
  /// CHECK-DAG: <<ConvC:c\d+>> TypeConversion [<<ConvB>>]
  /// CHECK-DAG:                Return [<<ConvC>>]

  /// CHECK-START: int Main.$noinline$loopPhiTwoStoreLoadConversions(int) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
  public static int $noinline$loopPhiTwoStoreLoadConversions(int value) {
      static_byte = (byte) value;
      // Irrelevant code but needed to make LSE use loop Phi placeholders.
      for (int q = 1; q < 12; q++) {
          unrelated_static_int = 24;
      }
      // Note: We need to go through `static_int` so that the instruction
      // simplifier eliminates the type conversion to `char`.
      // TODO: Improve the instruction simplifier to eliminate the conversion
      // for `static_char = (char) static_byte`.
      static_int = static_byte;
      static_char = (char) static_int;
      static_int = static_char;
      return static_int;
  }

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionInt8InLoop(int, boolean) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI:i\d+>>  StaticFieldGet field_name:Main.static_int field_type:Int32 loop:<<Loop:B\d+>>
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG: <<GetB:b\d+>>  StaticFieldGet field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetB>>] field_name:Main.static_int loop:<<Loop>>
  /// CHECK-DAG: <<GetI2:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:none
  /// CHECK-DAG:                Return [<<GetI2>>]

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionInt8InLoop(int, boolean) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<Phi1:i\d+>>  Phi [<<Value>>,<<Phi2:i\d+>>] loop:<<Loop:B\d+>>
  /// CHECK-DAG: <<Conv:b\d+>>  TypeConversion [<<Phi1>>] loop:<<Loop>>
  /// CHECK-DAG: <<Phi2>>       Phi [<<Phi1>>,<<Conv>>] loop:<<Loop>>
  /// CHECK-DAG:                Return [<<Phi1>>]

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionInt8InLoop(int, boolean) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
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

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionUint8InLoop(int, boolean) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI1:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:<<Loop:B\d+>>
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetI1>>] field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG: <<GetA:a\d+>>  StaticFieldGet field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetA>>] field_name:Main.static_int loop:<<Loop>>
  /// CHECK-DAG: <<GetI2:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:none
  /// CHECK-DAG:                Return [<<GetI2>>]

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionUint8InLoop(int, boolean) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<Phi1:i\d+>>  Phi [<<Value>>,<<Phi2:i\d+>>] loop:<<Loop:B\d+>>
  /// CHECK-DAG: <<Conv:a\d+>>  TypeConversion [<<Phi1>>] loop:<<Loop>>
  /// CHECK-DAG: <<Phi2>>       Phi [<<Phi1>>,<<Conv>>] loop:<<Loop>>
  /// CHECK-DAG:                Return [<<Phi1>>]

  /// CHECK-START: int Main.$noinline$conditionalStoreLoadConversionUint8InLoop(int, boolean) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
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

  /// CHECK-START: int Main.$noinline$twoConditionalStoreLoadConversionsInLoop(int, boolean) load_store_elimination (before)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<Value>>] field_name:Main.static_int
  /// CHECK-DAG: <<GetI1:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:<<Loop:B\d+>>
  // The type conversion has already been eliminated.
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetI1>>] field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG: <<GetB:b\d+>>  StaticFieldGet field_name:Main.static_byte loop:<<Loop>>
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetB>>] field_name:Main.static_int loop:<<Loop>>
  /// CHECK-DAG: <<GetI2:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:<<Loop>>
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetI2>>] field_name:Main.static_char loop:<<Loop>>
  /// CHECK-DAG: <<GetC:c\d+>>  StaticFieldGet field_name:Main.static_char loop:<<Loop>>
  /// CHECK-DAG:                StaticFieldSet [{{l\d+}},<<GetC>>] field_name:Main.static_int loop:<<Loop>>
  /// CHECK-DAG: <<GetI3:i\d+>> StaticFieldGet field_name:Main.static_int field_type:Int32 loop:none
  /// CHECK-DAG:                Return [<<GetI3>>]

  /// CHECK-START: int Main.$noinline$twoConditionalStoreLoadConversionsInLoop(int, boolean) load_store_elimination (after)
  /// CHECK-DAG: <<Value:i\d+>> ParameterValue
  /// CHECK-DAG: <<Phi1:i\d+>>  Phi [<<Value>>,<<Phi2:i\d+>>] loop:<<Loop:B\d+>>
  /// CHECK-DAG: <<Conv1:b\d+>> TypeConversion [<<Phi1>>] loop:<<Loop>>
  /// CHECK-DAG: <<Conv2:c\d+>> TypeConversion [<<Conv1>>] loop:<<Loop>>
  /// CHECK-DAG: <<Phi2>>       Phi [<<Phi1>>,<<Conv2>>] loop:<<Loop>>
  /// CHECK-DAG:                Return [<<Phi1>>]

  /// CHECK-START: int Main.$noinline$twoConditionalStoreLoadConversionsInLoop(int, boolean) load_store_elimination (after)
  /// CHECK-NOT: StaticFieldGet
  public static int $noinline$twoConditionalStoreLoadConversionsInLoop(int value, boolean cond) {
      static_int = value;
      for (int q = 1; q < 12; q++) {
          if (cond) {
              static_byte = (byte) static_int;
              // Note: We need to go through `static_int` so that the instruction
              // simplifier eliminates the type conversion to `char`.
              // TODO: Improve the instruction simplifier to eliminate the conversion
              // for `static_char = (char) static_byte`.
              static_int = static_byte;
              static_char = (char) static_int;
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

    assertIntEquals(42, $noinline$loopPhiStoreLoadConversionInt8(42));
    assertIntEquals(-42, $noinline$loopPhiStoreLoadConversionInt8(-42));
    assertIntEquals(-128, $noinline$loopPhiStoreLoadConversionInt8(128));
    assertIntEquals(127, $noinline$loopPhiStoreLoadConversionInt8(-129));

    assertIntEquals(42, $noinline$loopPhiStoreLoadConversionUint8(42));
    assertIntEquals(214, $noinline$loopPhiStoreLoadConversionUint8(-42));
    assertIntEquals(128, $noinline$loopPhiStoreLoadConversionUint8(128));
    assertIntEquals(127, $noinline$loopPhiStoreLoadConversionUint8(-129));

    assertIntEquals(42, $noinline$loopPhiTwoStoreLoadConversions(42));
    assertIntEquals(65494, $noinline$loopPhiTwoStoreLoadConversions(-42));
    assertIntEquals(65408, $noinline$loopPhiTwoStoreLoadConversions(128));
    assertIntEquals(127, $noinline$loopPhiTwoStoreLoadConversions(-129));
    assertIntEquals(0, $noinline$loopPhiTwoStoreLoadConversions(256));
    assertIntEquals(65535, $noinline$loopPhiTwoStoreLoadConversions(-257));

    assertIntEquals(42, $noinline$conditionalStoreLoadConversionInt8InLoop(42, false));
    assertIntEquals(42, $noinline$conditionalStoreLoadConversionInt8InLoop(42, true));
    assertIntEquals(-42, $noinline$conditionalStoreLoadConversionInt8InLoop(-42, false));
    assertIntEquals(-42, $noinline$conditionalStoreLoadConversionInt8InLoop(-42, true));
    assertIntEquals(128, $noinline$conditionalStoreLoadConversionInt8InLoop(128, false));
    assertIntEquals(-128, $noinline$conditionalStoreLoadConversionInt8InLoop(128, true));
    assertIntEquals(-129, $noinline$conditionalStoreLoadConversionInt8InLoop(-129, false));
    assertIntEquals(127, $noinline$conditionalStoreLoadConversionInt8InLoop(-129, true));

    assertIntEquals(42, $noinline$conditionalStoreLoadConversionUint8InLoop(42, false));
    assertIntEquals(42, $noinline$conditionalStoreLoadConversionUint8InLoop(42, true));
    assertIntEquals(-42, $noinline$conditionalStoreLoadConversionUint8InLoop(-42, false));
    assertIntEquals(214, $noinline$conditionalStoreLoadConversionUint8InLoop(-42, true));
    assertIntEquals(128, $noinline$conditionalStoreLoadConversionUint8InLoop(128, false));
    assertIntEquals(128, $noinline$conditionalStoreLoadConversionUint8InLoop(128, true));
    assertIntEquals(-129, $noinline$conditionalStoreLoadConversionUint8InLoop(-129, false));
    assertIntEquals(127, $noinline$conditionalStoreLoadConversionUint8InLoop(-129, true));

    assertIntEquals(42, $noinline$twoConditionalStoreLoadConversionsInLoop(42, false));
    assertIntEquals(42, $noinline$twoConditionalStoreLoadConversionsInLoop(42, true));
    assertIntEquals(-42, $noinline$twoConditionalStoreLoadConversionsInLoop(-42, false));
    assertIntEquals(65494, $noinline$twoConditionalStoreLoadConversionsInLoop(-42, true));
    assertIntEquals(128, $noinline$twoConditionalStoreLoadConversionsInLoop(128, false));
    assertIntEquals(65408, $noinline$twoConditionalStoreLoadConversionsInLoop(128, true));
    assertIntEquals(-129, $noinline$twoConditionalStoreLoadConversionsInLoop(-129, false));
    assertIntEquals(127, $noinline$twoConditionalStoreLoadConversionsInLoop(-129, true));
    assertIntEquals(256, $noinline$twoConditionalStoreLoadConversionsInLoop(256, false));
    assertIntEquals(0, $noinline$twoConditionalStoreLoadConversionsInLoop(256, true));
    assertIntEquals(-257, $noinline$twoConditionalStoreLoadConversionsInLoop(-257, false));
    assertIntEquals(65535, $noinline$twoConditionalStoreLoadConversionsInLoop(-257, true));
  }
}
