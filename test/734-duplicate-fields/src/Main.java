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

import java.lang.reflect.InvocationTargetException;

public class Main {

  public static void assertEquals(Object expected, Object actual) {
    if (!expected.equals(actual)) {
      throw new Error("Expected " + expected + ", got " + actual);
    }
  }

  public static void assertEquals(String expected, String actual) {
    if (!expected.equals(actual)) {
      throw new Error("Expected \"" + expected + "\", got \"" + actual + "\"");
    }
  }

  public static void main(String[] args) throws Exception {
    Class<?> cls = Class.forName("Cls");
    Object obj = cls.newInstance();
    Object instanceResult = cls.getDeclaredMethod("getInstanceField").invoke(obj, null);
    try {
      cls.getDeclaredMethod("getStaticField").invoke(null, null);
      throw new Error("Expected IncompatibleClassChangeError");
    } catch (InvocationTargetException e) {
      // Expected.
      assertEquals(e.getCause().getClass(), IncompatibleClassChangeError.class);
    }

    assertEquals(0x42, instanceResult);
    assertEquals("public int Cls.myField", cls.getDeclaredField("myField").toString());
  }
}
