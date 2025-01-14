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

    public static void main(String args[]) {
        System.out.print("IfXLtzAElseB(-7): ");
        $noinline$IfXLtzAElseB(-7);

        System.out.print("IfXLtzAElseB(42): ");
        $noinline$IfXLtzAElseB(42);

        System.out.print("IfXLtzAElseB_Move(-7): ");
        new Main().$noinline$IfXLtzAElseB_Move(-7);

        System.out.print("IfXLtzAElseB_Move(42): ");
        new Main().$noinline$IfXLtzAElseB_Move(42);
    }

    private static void $noinline$A() {
        System.out.println("A");
    }

    private static void $noinline$B() {
        System.out.println("B");
    }

    private static boolean $inline$XLtz(int x) {
      return x < 0;
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
        // After inining, we shall create a `HSelect` and then simplify it as `HLessThan`.
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
}
