/*
 * Copyright (C) 2023 The Android Open Source Project
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

import testpackage.TestNestedClasses;

public class Main {
    public static void main(String[] args) {
        testNestedClass();
    }

    private static void testNestedClass() {
        TestNestedClasses host = new TestNestedClasses();
        check("host.testInnerAccess", 4, host.testInnerAccess());
        check("host.testInnerAccess", 8, host.testInnerAccess());
        check("host.testInnerAccess", 12, host.testInnerAccess());

        host.reset();
        check("innerA.testHostAccess", 2, host.innerA.testHostAccess());
        check("innerA.testHostAccess", 4, host.innerA.testHostAccess());
        check("innerA.testHostAccess", 6, host.innerA.testHostAccess());
        check("innerA.testMemberAccess", 2, host.innerA.testMemberAccess());
        check("innerA.testMemberAccess", 4, host.innerA.testMemberAccess());
        check("innerA.testMemberAccess", 6, host.innerA.testMemberAccess());

        host.reset();
        check("innerB.testHostAccess", 2, host.innerB.testHostAccess());
        check("innerB.testHostAccess", 4, host.innerB.testHostAccess());
        check("innerB.testHostAccess", 6, host.innerB.testHostAccess());
        check("innerB.testMemberAccess", 2, host.innerB.testMemberAccess());
        check("innerB.testMemberAccess", 4, host.innerB.testMemberAccess());
        check("innerB.testMemberAccess", 6, host.innerB.testMemberAccess());
    }

    private static void check(String msg, int actual, int expected) {
        if (actual != expected) {
            System.out.println(msg + " : " + actual + " != " + expected);
            System.exit(1);
        }
    }
}
