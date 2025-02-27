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

package com.android.ahat.heapdump;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.stream.Collectors;


public class DuplicateStrings {

    /**
     * Represents information about a duplicated string, including its total size and count.
     */
    public static class DuplicatedString {
        private Size size;
        private int count;
        private String content;

        /**
         * Constructs a new DuplicatedString instance.
         *
         * @param size  The total size of the duplicated string instances.
         * @param count The number of times the string is duplicated.
         * @param content The content of the string.
         */
        public DuplicatedString(Size size, int count, String content) {
            this.size = size;
            this.count = count;
            this.content = content;
        }

        /**
         * Returns the total size of the duplicated string instances.
         *
         * @return The total size.
         */
        public Size getSize() {
            return size;
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
         * Returns the number of times the string is duplicated.
         *
         * @return The duplication count.
         */
        public String getContent() {
            return content;
        }

    }

    /**
     * Identifies duplicated string instances within a heap dump.
     *
     * @param root      The root of the heap dump.
     * @param instances The collection of instances to analyze.
     * @return A map where keys are the duplicated strings and values are their corresponding
     * {@link DuplicatedString} objects.
     */
    public static Map<String, DuplicatedString> findDuplicatedStrings(SuperRoot root, Instances<AhatInstance> instances) {
        Map<String, DuplicatedString> duplicatedStrings = new HashMap<>();

        for (AhatInstance obj : instances) {
            if (obj.isUnreachable() || !obj.getClassName().equals("java.lang.String") ) {
                continue;
            }
            String str = obj.asString();
            if (str == null || str == "") {
                continue;
            }
            Size size = obj.getSize();
            if (duplicatedStrings.containsKey(str)) {
                DuplicatedString existingString = duplicatedStrings.get(str);
                existingString.size = existingString.size.plus(size);
                existingString.count = existingString.count += 1;
                duplicatedStrings.put(str, existingString);
            } else {
                duplicatedStrings.put(str, new DuplicatedString(size, 1, str));
            }
        }
        return duplicatedStrings;
    }

    /**
     * Finds the top most frequently duplicated strings.
     *
     * @param duplicateStrings A map of duplicated strings and their corresponding {@link DuplicatedString} objects.
     * @return A list of entries representing the top most duplicated strings, sorted by count in descending order.
     * Returns null if the input map is null.
     */
    public static List<DuplicatedString> findMostDuplicatedStrings(Map<String, DuplicatedString> duplicateStrings, int limit) {
        if (duplicateStrings == null || duplicateStrings.isEmpty()) {
            return null;
        }
        return duplicateStrings.values().stream()
                .sorted(Comparator.comparingInt(DuplicatedString::getCount).reversed())
                .limit(limit)
                .collect(Collectors.toList());
    }
}