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
    private int intField;

    public static void main(String[] args) {
        System.out.print("IfXLtzAElseB(-7): ");
        $noinline$IfXLtzAElseB(-7);
        System.out.print("IfXLtzAElseB(42): ");
        $noinline$IfXLtzAElseB(42);

        System.out.print("IfXLtzAElseB_Move(-7): ");
        new Main().$noinline$IfXLtzAElseB_Move(-7);
        System.out.print("IfXLtzAElseB_Move(42): ");
        new Main().$noinline$IfXLtzAElseB_Move(42);

        System.out.print("IfXLtzAElseB_EnvUse(-7): ");
        $noinline$IfXLtzAElseB_EnvUse(-7);
        System.out.print("IfXLtzAElseB_EnvUse(42): ");
        $noinline$IfXLtzAElseB_EnvUse(42);

        System.out.print("IfXNullAElseB(null): ");
        $noinline$IfXNullAElseB(null);
        System.out.print("IfXNullAElseB(new Object()): ");
        $noinline$IfXNullAElseB(new Object());

        System.out.print("IfXNullAElseB_Move(null): ");
        new Main().$noinline$IfXNullAElseB_Move(null);
        System.out.print("IfXNullAElseB_Move(new Object()): ");
        new Main().$noinline$IfXNullAElseB_Move(new Object());

        System.out.print("IfXNullAElseB_EnvUse(null): ");
        new Main().$noinline$IfXNullAElseB_EnvUse(null);
        System.out.print("IfXNullAElseB_EnvUse(new Object()): ");
        new Main().$noinline$IfXNullAElseB_EnvUse(new Object());

        System.out.print("IfXNullAElseB_RefNoEnvInBlock(null, true): ");
        new Main().$noinline$IfXNullAElseB_RefNoEnvInBlock(null, true);
        System.out.print("IfXNullAElseB_RefNoEnvInBlock(new Object(), true): ");
        new Main().$noinline$IfXNullAElseB_RefNoEnvInBlock(new Object(), true);
        System.out.print("IfXNullAElseB_RefNoEnvInBlock(null, false): ");
        new Main().$noinline$IfXNullAElseB_RefNoEnvInBlock(null, false);
        System.out.print("IfXNullAElseB_RefNoEnvInBlock(new Object(), false): ");
        new Main().$noinline$IfXNullAElseB_RefNoEnvInBlock(new Object(), false);

        // Note: We do not test the code paths where `ConditionMoveWouldExtendReferenceLifetime()`
        // in the "prepare_for_register_allocation" pass finds an instruction with environment
        // between the `HCondition` and its user in this run-test. These are difficult to create
        // from Java code and changes to other passes can easily invalidate such tests. Therefore
        // we defer to using gtests for these cases.
    }

    private static void $noinline$A() {
        System.out.println("A");
    }

    private static void $noinline$B() {
        System.out.println("B");
    }

    private static void $noinline$C() {
        System.out.println("C");
    }

    private static boolean $inline$XLtz(int x) {
        // After inlining, this shall be turned to a `HSelect` and then simplified as `HLessThan`.
        return x < 0;
    }

    private static boolean $inline$XNull(Object x) {
        // After inlining, this shall be turned to a `HSelect` and then simplified as `HEqual`.
        return x == null;
    }

    private static void $noinline$ignore(int ignored) {}

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB(int) prepare_for_register_allocation (before)
    /// CHECK:      <<Cond:z\d+>> {{GreaterThanOrEqual|LessThan}} emitted_at_use_site:false
    /// CHECK-NEXT:               If [<<Cond>>]

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB(int) prepare_for_register_allocation (after)
    /// CHECK:      <<Cond:z\d+>> {{GreaterThanOrEqual|LessThan}} emitted_at_use_site:true
    /// CHECK-NEXT:               If [<<Cond>>]

    public static void $noinline$IfXLtzAElseB(int x) {
        if (x < 0) {
            $noinline$A();
        } else {
            $noinline$B();
        }
    }

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB_Move(int) prepare_for_register_allocation (before)
    /// CHECK:      <<Cond:z\d+>> LessThan emitted_at_use_site:false
    /// CHECK-NEXT:               InstanceFieldGet
    // On X86, there can be also X86ComputeBaseMethodAddress here.
    /// CHECK:                    If [<<Cond>>]

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB_Move(int) prepare_for_register_allocation (after)
    /// CHECK:                    InstanceFieldGet
    // On X86, there can be also X86ComputeBaseMethodAddress here.
    /// CHECK:      <<Cond:z\d+>> LessThan emitted_at_use_site:true
    /// CHECK-NEXT:               If [<<Cond>>]

    public void $noinline$IfXLtzAElseB_Move(int x) {
        boolean cond = $inline$XLtz(x);

        int value = intField;
        if (cond) {
            cond = false;  // Avoid environment use below.
            $noinline$A();
        } else {
            cond = false;  // Avoid environment use below.
            $noinline$B();
        }
        $noinline$ignore(value);
    }

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB_EnvUse(int) prepare_for_register_allocation (before)
    /// CHECK:                    LessThan emitted_at_use_site:false

    /// CHECK-START: void Main.$noinline$IfXLtzAElseB_EnvUse(int) prepare_for_register_allocation (after)
    /// CHECK:                    LessThan emitted_at_use_site:false

    public static void $noinline$IfXLtzAElseB_EnvUse(int x) {
        boolean cond = $inline$XLtz(x);
        if (cond) {
            $noinline$A();
        } else {
            $noinline$B();
        }
    }

    /// CHECK-START: void Main.$noinline$IfXNullAElseB(java.lang.Object) prepare_for_register_allocation (before)
    /// CHECK:      <<Cond:z\d+>> {{Equal|NotEqual}} emitted_at_use_site:false
    /// CHECK-NEXT:               If [<<Cond>>]

    /// CHECK-START: void Main.$noinline$IfXNullAElseB(java.lang.Object) prepare_for_register_allocation (after)
    /// CHECK:      <<Cond:z\d+>> {{Equal|NotEqual}} emitted_at_use_site:true
    /// CHECK-NEXT:               If [<<Cond>>]

    public static void $noinline$IfXNullAElseB(Object x) {
        if (x == null) {
            $noinline$A();
        } else {
            $noinline$B();
        }
    }

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_Move(java.lang.Object) prepare_for_register_allocation (before)
    /// CHECK:      <<Cond:z\d+>> Equal emitted_at_use_site:false
    /// CHECK-NEXT:               InstanceFieldGet
    // On X86, there can be also X86ComputeBaseMethodAddress here.
    /// CHECK:                    If [<<Cond>>]

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_Move(java.lang.Object) prepare_for_register_allocation (after)
    /// CHECK:                    InstanceFieldGet
    // On X86, there can be also X86ComputeBaseMethodAddress here.
    /// CHECK:      <<Cond:z\d+>> Equal emitted_at_use_site:true
    /// CHECK-NEXT:               If [<<Cond>>]

    public void $noinline$IfXNullAElseB_Move(Object x) {
        boolean cond = $inline$XNull(x);

        int value = intField;
        if (cond) {
            cond = false;  // Avoid environment use below.
            $noinline$A();
        } else {
            cond = false;  // Avoid environment use below.
            $noinline$B();
        }
        $noinline$ignore(value);
    }

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_EnvUse(java.lang.Object) prepare_for_register_allocation (before)
    /// CHECK:                    Equal emitted_at_use_site:false

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_EnvUse(java.lang.Object) prepare_for_register_allocation (after)
    /// CHECK:                    Equal emitted_at_use_site:false

    public static void $noinline$IfXNullAElseB_EnvUse(Object x) {
        boolean cond = $inline$XNull(x);
        if (cond) {
            $noinline$A();
        } else {
            $noinline$B();
        }
    }

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_RefNoEnvInBlock(java.lang.Object, boolean) prepare_for_register_allocation (before)
    /// CHECK:      <<Cond:z\d+>> {{Equal|NotEqual}} emitted_at_use_site:false
    /// CHECK:                    If [<<Cond>>]

    /// CHECK-START: void Main.$noinline$IfXNullAElseB_RefNoEnvInBlock(java.lang.Object, boolean) prepare_for_register_allocation (after)
    /// CHECK:      <<Cond:z\d+>> {{Equal|NotEqual}} emitted_at_use_site:false
    /// CHECK:                    If [<<Cond>>]

    public static void $noinline$IfXNullAElseB_RefNoEnvInBlock(Object x, boolean otherCond) {
        boolean cond = $inline$XNull(x);
        if (otherCond) {
            if (cond) {
                cond = false;  // Avoid environment use below.
                $noinline$A();
            } else {
                cond = false;  // Avoid environment use below.
                $noinline$B();
            }
        } else {
            cond = false;  // Avoid environment use below.
            $noinline$C();
        }
    }
}
