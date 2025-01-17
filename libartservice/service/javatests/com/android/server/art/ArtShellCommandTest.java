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

package com.android.server.art;

import static com.google.common.truth.Truth.assertThat;

import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.when;

import android.os.Process;

import androidx.test.filters.SmallTest;

import com.android.server.art.prereboot.PreRebootDriver;
import com.android.server.art.prereboot.PreRebootStatsReporter;
import com.android.server.art.testing.CommandExecution;
import com.android.server.pm.PackageManagerLocal;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;

import java.util.stream.Collectors;

@SmallTest
@RunWith(MockitoJUnitRunner.StrictStubs.class)
public class ArtShellCommandTest {
    @Mock private PreRebootDriver mPreRebootDriver;
    @Mock private PreRebootStatsReporter mPreRebootStatsReporter;
    @Mock private PreRebootDexoptJob.Injector mPreRebootDexoptJobInjector;
    @Mock private ArtManagerLocal.Injector mArtManagerLocalInjector;
    @Mock private PackageManagerLocal mPackageManagerLocal;
    @Mock private ArtShellCommand.Injector mInjector;

    private PreRebootDexoptJob mPreRebootDexoptJob;
    private ArtManagerLocal mArtManagerLocal;
    private ArtShellCommand mArtShellCommand;

    @Before
    public void setUp() throws Exception {
        lenient()
                .when(mPreRebootDexoptJobInjector.getPreRebootDriver())
                .thenReturn(mPreRebootDriver);
        lenient()
                .when(mPreRebootDexoptJobInjector.getStatsReporter())
                .thenReturn(mPreRebootStatsReporter);
        mPreRebootDexoptJob = new PreRebootDexoptJob(mPreRebootDexoptJobInjector);

        lenient()
                .when(mArtManagerLocalInjector.getPreRebootDexoptJob())
                .thenReturn(mPreRebootDexoptJob);
        mArtManagerLocal = new ArtManagerLocal(mArtManagerLocalInjector);

        lenient().when(mInjector.getArtManagerLocal()).thenReturn(mArtManagerLocal);
        lenient().when(mInjector.getPackageManagerLocal()).thenReturn(mPackageManagerLocal);
        lenient().when(mInjector.getCallingUid()).thenReturn(Process.SHELL_UID);
        mArtShellCommand = new ArtShellCommand(mInjector);
    }

    @Test
    public void testOnOtaStaged() throws Exception {
        when(mInjector.getCallingUid()).thenReturn(Process.ROOT_UID);

        try (var execution = new CommandExecution(
                     mArtShellCommand, "art", "pr-dexopt-job", "--version")) {
            int exitCode = execution.waitAndGetExitCode();
            assertThat(execution.getStdout().readLine()).isEqualTo("3");
            assertThat(exitCode).isEqualTo(0);
        }
    }
}
