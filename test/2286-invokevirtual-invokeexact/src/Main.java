import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Method;

public class Main {
    public static void main(String[] args) throws Throwable {
        MethodHandle mh = MethodHandles.lookup()
            .findStatic(Main.class, "toString", MethodType.methodType(Object.class, Object[].class));

        Object[] objects = new Object[2];
        objects[0] = "111";
        objects[1] = 10;

        Class testClass = Class.forName("Test");

        Method m = testClass.getDeclaredMethod("test", MethodHandle.class, Object[].class);
        Object o = m.invoke(null, mh, objects);

        System.out.println(o);
    }

    static Object toString(Object[] objects) {
        return "objects";
    }
}
