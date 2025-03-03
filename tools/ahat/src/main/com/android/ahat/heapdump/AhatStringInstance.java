/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.ahat.heapdump;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.TreeMap;
import java.util.Comparator;
import java.util.stream.Collectors;

/**
 * A java object that represents duplicated strings found in the heap dump.
 */
public class AhatStringInstance extends AhatClassInstance implements Comparable<AhatStringInstance> {

    AhatStringInstance(long id) {
        super(id);
    }

    @Override
    public boolean isDuplicatedStringInstance() {
        return true;
    }

    @Override
    public AhatStringInstance asDuplicatedStringInstance() {
        return this;
    }

    /**
     * Simple order for all duplicated string instances on TreeMultimap
     */
    @Override
    public int compareTo(AhatStringInstance other) {
        return Long.compare(this.getId(), other.getId());
    }

    /**
     * Parsed information for duplicated strings dumped in the heapdump
     */
    public static class DuplicatedStringData {
        private String content;
        private int count;
        private List<AhatStringInstance> instances;

        DuplicatedStringData(String content, int count, List<AhatStringInstance> instances) {
            this.content = content;
            this.count = count;
            this.instances = instances;
        }

        /**
         * Returns the instances.
         *
         * @return The list of instances.
         */
        public List<AhatStringInstance> getInstances() {
            return instances;
        }

        /**
         * Returns the number of times the string is duplicated.
         *
         * @return The duplication count.
         */
        public int getCount() {
            return count;
        }

        /**
         * Returns the content of the string.
         *
         * @return The duplicated string contents.
         */
        public String getContent() {
            return content;
        }
    }

    /**
     * Find the string data that is included in the heap dump
     *
     * @param root      root of the heap dump
     * @param instances all the instances from where the duplicated string data will be excluded
     * @return A Map of string contents with the instances that use that string.
     */
    public static Map<String, List<AhatStringInstance>> findStringData(SuperRoot root, Instances<AhatInstance> instances) {
        Map<String, List<AhatStringInstance>> duplicates = new HashMap<>();

        for (AhatInstance obj : instances) {
            if (obj.isUnreachable() || !obj.getClassName().equals("java.lang.String")) {
                continue;
            }
            AhatStringInstance dupString = obj.asDuplicatedStringInstance();
            if (dupString == null) {
                continue;
            }

            String str = obj.asString();
            if (str == null) {
                continue;
            }
            duplicates.computeIfAbsent(str, k -> new ArrayList<>()).add(dupString);
        }
        return duplicates;
    }

    /**
     * Finds the most frequently duplicated strings.
     *
     * @param stringData The data containing duplicated strings.
     * @param limit                The maximum number of duplicated strings to return.
     * @return A list of entries representing the top most duplicated strings,
     * sorted by count in descending order.
     * Returns null if the input data is null.
     */
    public static List<DuplicatedStringData> findMostDuplicatedStrings(
            Map<String, List<AhatStringInstance>> stringData, int limit) {
        if (stringData == null) {
            return null;
        }
        return stringData.entrySet().stream()
                .sorted(Comparator.comparing((Map.Entry<String, List<AhatStringInstance>> entry) -> entry.getValue().size()).reversed())
                .limit(limit)
                .map(entry -> {
                    String content = entry.getKey();
                    List<AhatStringInstance> instances = entry.getValue();
                    int count = instances.size();
                    return new DuplicatedStringData(content, count, instances);
                })
                .collect(Collectors.toList());
    }
}