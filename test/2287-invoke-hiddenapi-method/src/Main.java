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

import java.lang.reflect.Method;
import dalvik.system.VMRuntime;

public class Main {
    public static void main(String[] args) throws Throwable {
        System.loadLibrary(args[0]);
        enableHiddenApiChecks();
        VMRuntime.getRuntime().setTargetSdkVersion(28);
        Class testClass = Class.forName("Test");

        Method m = testClass.getDeclaredMethod("test", null);
        String str = (String) m.invoke(null);
        if (!str.isEmpty()) {
            throw new AssertionError("not empty: " + str);
        }

        Class stringFactory = Class.forName("java.lang.StringFactory");
        try {
            Method emptyStringMethod = stringFactory.getDeclaredMethod("newEmptyString", null);
            throw new AssertionError("unreachable");
        } catch (Exception expected) {

        }
    }

    static Object toString(Object[] objects) {
        return "objects";
    }

    // Definition is in 2270-mh-internal-hiddenapi.
    private static native void enableHiddenApiChecks();
}
