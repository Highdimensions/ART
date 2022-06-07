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

package testpackage;

public class TestNestedClasses {
    public InnerA innerA = new InnerA();
    public InnerB innerB = new InnerB();
    private int hostVal = 0;

    public int testInnerAccess() {
        ++innerA.val;
        innerA.inc();
        ++innerB.val;
        innerB.inc();
        return innerA.val + innerB.val;
    }

    public void reset() {
        hostVal = 0;
        innerA.val = 0;
        innerB.val = 0;
    }

    private void hostInc() {
        ++hostVal;
    }

    public class InnerA {

        private int val = 0;

        public int testMemberAccess() {
            ++innerB.val;
            innerB.inc();
            return innerB.val;
        }

        public int testHostAccess() {
            ++hostVal;
            hostInc();
            return hostVal;
        }

        private void inc() {
            ++this.val;
        }
    }

    public class InnerB {

        private int val = 0;

        public int testMemberAccess() {
            ++innerA.val;
            innerA.inc();
            return innerA.val;
        }

        public int testHostAccess() {
            ++hostVal;
            hostInc();
            return hostVal;
        }

        private void inc() {
            ++this.val;
        }
    }
}
