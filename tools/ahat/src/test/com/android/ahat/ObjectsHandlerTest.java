/*
 * Copyright (C) 2018 The Android Open Source Project
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

package com.android.ahat;

import com.android.ahat.heapdump.AhatInstance;
import com.android.ahat.heapdump.AhatSnapshot;
import com.android.ahat.heapdump.Site;
import java.io.IOException;
import java.util.List;
import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public class ObjectsHandlerTest {
  @Test
  public void getObjects() throws IOException {
    TestDump dump = TestDump.getTestDump();
    AhatSnapshot snapshot = dump.getAhatSnapshot();

    Site root = snapshot.getRootSite();

    // We expect a single instance of DumpedStuff
    List<AhatInstance> dumped = ObjectsHandler.getObjects(
        root, "DumpedStuff", /* subclass */ false, /* heapName */ null, /* stringHash */ null);
    assertEquals(1, dumped.size());
    assertTrue(dumped.get(0).getClassName().equals("DumpedStuff"));

    // We expect no direct instances of SuperDumpedStuff
    List<AhatInstance> direct = ObjectsHandler.getObjects(
        root, "SuperDumpedStuff", /* subclass */ false, /* heapName */ null, /* stringHash */ null);
    assertTrue(direct.isEmpty());

    // We expect one subclass instance of SuperDumpedStuff
    List<AhatInstance> subclass = ObjectsHandler.getObjects(
        root, "SuperDumpedStuff", /* subclass */ true, /* heapName */ null, /* stringHash */ null);
    assertEquals(1, subclass.size());
    assertTrue(subclass.get(0).getClassName().equals("DumpedStuff"));
    assertEquals(dumped.get(0), subclass.get(0));

    // We expect there to be no instances with the hash 123.
    List<AhatInstance> invalidHash = ObjectsHandler.getObjects(
        root, "java.lang.String", /* subclass */ false, /* heapName */ null, /* stringHash */ "123");
    assertEquals(0, invalidHash.size());

    // We expect there to be one instance with a string value of os.version.
    List<AhatInstance> stringHash = ObjectsHandler.getObjects(
        root, "java.lang.String", /* subclass */ false, /* heapName */ null, /* stringHash */ "1174476494");
    assertEquals(1, stringHash.size());
    assertTrue(stringHash.get(0).getClassName().equals("java.lang.String"));
    assertTrue(stringHash.get(0).asString().equals("os.version"));

    // We expect stringHash to be null due to the wrong class name.
    List<AhatInstance> wrongClass = ObjectsHandler.getObjects(
        root, "DumpedStuff", /* subclass */ true, /* heapName */ null, /* stringHash */ "1174476494");
    assertEquals(1, wrongClass.size());
    assertTrue(wrongClass.get(0).getClassName().equals("DumpedStuff"));
  }
}
