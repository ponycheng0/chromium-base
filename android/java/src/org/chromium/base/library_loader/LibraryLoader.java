// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.base.library_loader;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;

import androidx.annotation.IntDef;
import androidx.annotation.VisibleForTesting;

import org.chromium.base.BaseSwitches;
import org.chromium.base.CommandLine;
import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.NativeLibraryLoadedStatus;
import org.chromium.base.NativeLibraryLoadedStatus.NativeLibraryLoadedStatusProvider;
import org.chromium.base.StrictModeContext;
import org.chromium.base.TimeUtils.CurrentThreadTimeMillisTimer;
import org.chromium.base.TimeUtils.UptimeMillisTimer;
import org.chromium.base.TraceEvent;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.base.annotations.NativeMethods;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.base.metrics.UmaRecorderHolder;
import org.chromium.build.BuildConfig;
import org.chromium.build.NativeLibraries;
import org.chromium.build.annotations.MainDex;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

import javax.annotation.concurrent.GuardedBy;

/**
 * This class provides functionality to load and register the native libraries.
 * Callers are allowed to separate loading the libraries from initializing
 * them. When a zygote process is used (WebView or AppZygote) the per process
 * initialization happens after the application processes are forked from the
 * zygote process.
 *
 * The libraries may be loaded and initialized from any thread. Synchronization
 * primitives are used to ensure that overlapping requests from different
 * threads are handled sequentially.
 *
 * See also base/android/library_loader/library_loader_hooks.cc, which contains
 * the native counterpart to this class.
 */
@MainDex
@JNINamespace("base::android")
public class LibraryLoader {
    private static final String TAG = "LibraryLoader";

    // Constant guarding debug logging in this class.
    static final boolean DEBUG = false;

    // Shared preferences key for the reached code profiler.
    private static final String DEPRECATED_REACHED_CODE_PROFILER_KEY =
            "reached_code_profiler_enabled";
    private static final String REACHED_CODE_SAMPLING_INTERVAL_KEY =
            "reached_code_sampling_interval";

    // Compile time switch for sharing RELRO between the browser and the app zygote.
    // TODO(crbug.com/1154224): remove when the issue is closed.
    private static final boolean ALLOW_CHROMIUM_LINKER_IN_ZYGOTE = true;

    // Default sampling interval for reached code profiler in microseconds.
    private static final int DEFAULT_REACHED_CODE_SAMPLING_INTERVAL_US = 10000;

    // Shared preferences key for the background thread pool setting.
    private static final String BACKGROUND_THREAD_POOL_KEY = "background_thread_pool_enabled";

    // The singleton instance of LibraryLoader. Never null (not final for tests).
    private static LibraryLoader sInstance = new LibraryLoader();

    private static boolean sBrowserStartupBlockedForTesting;

    // Helps mInitializedForTesting and mLoadedForTesting to be removed by R8.
    private static boolean sEnableStateForTesting;

    // One-way switch becomes true when the libraries are initialized (by calling
    // LibraryLoaderJni.get().libraryLoaded, which forwards to LibraryLoaded(...) in
    // library_loader_hooks.cc). Note that this member should remain a one-way switch, since it
    // accessed from multiple threads without a lock.
    private volatile boolean mInitialized;

    // One way switch used by initInAppZygote() when the current platform does not support loading
    // using a Chromium Linker in the App Zygote. Because of this limited usage it can avoid
    // synchronization.
    private boolean mFallbackToSystemLinker;

    // State that only transitions from false->true. Volatile for the same reasons as mInitialized.
    private volatile boolean mLoaded;

    // Tracks mLoaded, but can be reset to NOT_LOADED between tests to ensure that each test that
    // requires native explicitly loads it.
    private boolean mLoadedForTesting;

    // Tracks mInitialized, but can be reset to false between tests to ensure that each test that
    // requires native explicitly loads it.
    private boolean mInitializedForTesting;

    // Whether to use the Chromium linker vs. the system linker.
    // Avoids locking: should be initialized very early.
    private boolean mUseChromiumLinker = NativeLibraries.sUseLinker;

    // The type of process the shared library is loaded in. Gets passed to native after loading.
    // Avoids locking: should be initialized very early.
    private @LibraryProcessType int mLibraryProcessType;

    // Mediates all communication between Linker instances in different processes.
    private final MultiProcessMediator mMediator = new MultiProcessMediator();

    // Guards all the fields below.
    private final Object mLock = new Object();

    // When a Chromium linker is used, this field represents the concrete class serving as a Linker.
    // Always accessed via getLinker() because the choice of the class can be influenced by
    // public setLinkerImplementation() below.
    @GuardedBy("mLock")
    private Linker mLinker;

    @GuardedBy("mLock")
    private NativeLibraryPreloader mLibraryPreloader;

    @GuardedBy("mLock")
    private boolean mLibraryPreloaderCalled;

    // Similar to |mLoaded| but is limited case of being loaded in app zygote.
    // This is exposed to clients.
    @GuardedBy("mLock")
    private boolean mLoadedByZygote;

    // One-way switch becomes true when the Java command line is switched to
    // native.
    @GuardedBy("mLock")
    private boolean mCommandLineSwitched;

    // Enumeration telling which init* methods were used, and therefore
    // which process the library is loaded in.
    @IntDef({CreatedIn.MAIN, CreatedIn.ZYGOTE, CreatedIn.CHILD_WITHOUT_ZYGOTE})
    @Retention(RetentionPolicy.SOURCE)
    private @interface CreatedIn {
        int MAIN = 0;
        int ZYGOTE = 1;
        int CHILD_WITHOUT_ZYGOTE = 2;
    }

    // Returns true when sharing RELRO between the browser process and the app zygote should *not*
    // be attempted.
    public static boolean mainProcessIntendsToProvideRelroFd() {
        return !ALLOW_CHROMIUM_LINKER_IN_ZYGOTE || Build.VERSION.SDK_INT <= Build.VERSION_CODES.R;
    }

    /**
     * Inner class encapsulating points of communication between instances of LibraryLoader in
     * different processes.
     *
     * Usage:
     *
     * 0. In the main (Browser) process this mediator can be bypassed by
     *    {@link LibraryLoader#ensureInitialized()}. It is convenient for targets that do not pay
     *    attention to RELRO sharing and load time statistics, but it is also more error prone. The
     *    {@link #ensureInitializedInMainProcess()} is recommended.
     *
     * 1. For a {@link LibraryLoader} requiring the knowledge of the load address before
     *    initialization, {@link #takeLoadAddressFromBundle(Bundle)} should be called first. It is
     *    done very early after establishing a Binder connection.
     *
     * 2. After the load address is received, the object needs to be initialized using one of
     *    {@link #ensureInitializedInMainProcess()}, {@link #initInChildProcess()} and
     *    {@link #initInAppZygote()}. For the main process the subsequent calls to initialization
     *    are ignored, primarily to simplify tests.
     *
     * 3. Later {@link #putLoadAddressToBundle(Bundle)} and
     *    {@link #takeLoadAddressFromBundle(Bundle)} should be called for passing the RELRO
     *    information between library loaders.
     *
     * Internally the {@link LibraryLoader} may ignore these messages because it can fall back to
     * not sharing RELRO.
     *
     * In general the class is *not* thread safe. The client must guarantee that the steps 1-3 above
     * happen sequentially in the memory model sense. After that the class is safe to use from
     * multiple threads concurrently.
     */
    public class MultiProcessMediator {
        // Currently clients initialize |mLoadAddress| strictly before any other method can get
        // executed on a different thread. Hence, synchronization is not required.
        private long mLoadAddress;

        // Only ever switched from false to true.
        private volatile boolean mInitDone;

        // How the mediator was created. The LibraryLoader.ensureInitialized() uses this default
        // value.
        private volatile @CreatedIn int mCreatedIn = CreatedIn.MAIN;

        /**
         * Extracts the load address as provided by another process.
         * @param bundle The Bundle to extract from.
         */
        public void takeLoadAddressFromBundle(Bundle bundle) {
            assert !mInitDone;
            mLoadAddress = Linker.extractLoadAddressFromBundle(bundle);
        }

        private long getLoadAddress() {
            synchronized (mLock) {
                return mLoadAddress;
            }
        }

        /**
         * Initializes the Main (Browser) process side of communication. This process coordinates
         * creation of other processes. Can be called more than once, subsequent calls are ignored.
         */
        public void ensureInitializedInMainProcess() {
            if (mInitDone) return;
            if (useChromiumLinker()) {
                boolean attemptProduceRelro = mainProcessIntendsToProvideRelroFd();
                // When the main process creates the shared region with relocations, it is faster
                // to randomize the load address than to find the reserved one
                // in /proc. When the main process relies on RELRO from the
                // zygote, then it should scan /proc to find the reserved range
                // because waiting for zygote to reveal its address would have
                // delayed startup.
                if (DEBUG) {
                    Log.i(TAG, "ensureInitializedInMainProcess, producing RELRO FD: %b",
                            attemptProduceRelro);
                }
                // For devices avoiding the App Zygote in
                // ChildConnectionAllocator.createVariableSize() the FIND_RESERVED search can be
                // avoided: a random region is sufficient. TODO(pasko): Investigate whether it is
                // worth coordinating with the ChildConnectionAllocator. To speed up process
                // creation.
                int preferAddress = attemptProduceRelro ? Linker.PreferAddress.RESERVE_RANDOM
                                                        : Linker.PreferAddress.FIND_RESERVED;
                getLinker().ensureInitialized(
                        attemptProduceRelro, preferAddress, /* addressHint= */ 0);
            }
            mCreatedIn = CreatedIn.MAIN;
            mInitDone = true;
        }

        /**
         * Serializes the load address for communication, if any was determined during
         * initialization. Must be called after the library has been loaded in this process.
         * @param bundle Bundle to put the address to.
         */
        public void putLoadAddressToBundle(Bundle bundle) {
            assert mInitDone;
            if (useChromiumLinker()) {
                getLinker().putLoadAddressToBundle(bundle);
            }
        }

        /**
         * Initializes in the App Zygote process. Will be followed by initInChildProcess() in all
         * processes inheriting from the app zygote.
         */
        public void initInAppZygote() {
            assert !mInitDone;
            if (useChromiumLinker() && !mainProcessIntendsToProvideRelroFd()) {
                getLinker().ensureInitialized(
                        /* asRelroProducer= */ true, Linker.PreferAddress.FIND_RESERVED, 0);
            } else {
                // The main process will attempt to create RELRO FD without coordination. Fall back
                // to loading with the system linker. Can happen in tests and on dev builds with
                // forceSystemLinker(), should not happen in the field.
                mFallbackToSystemLinker = true;
            }
            mCreatedIn = CreatedIn.ZYGOTE;
            // The initInChildProcess() will set |mInitDone| to |true| after fork(2).
        }

        /**
         * Initializes in processes other than "Main". Can be called only once in each non-main
         * process.
         */
        public void initInChildProcess() {
            assert !mInitDone;
            if (!useChromiumLinker()) {
                mInitDone = true;
                return;
            }
            if (mainProcessIntendsToProvideRelroFd()) {
                if (DEBUG) {
                    Log.i(TAG, "initInChildProcess: RELRO FD not provided by App Zygote");
                }
                getLinker().ensureInitialized(/* asRelroProducer= */ false,
                        Linker.PreferAddress.RESERVE_HINT, getLoadAddress());
            } else if (isLoadedByZygote()) {
                if (DEBUG) {
                    Log.i(TAG,
                            "initInChildProcess: already loaded by app zygote "
                                    + "(mFallbackToSystemLinker=%b)",
                            mFallbackToSystemLinker);
                }
            } else if (mCreatedIn == CreatedIn.ZYGOTE) {
                if (DEBUG) {
                    Log.i(TAG, "initInChildProcess: the app zygote failed to produce RELRO FD");
                }
                getLinker().ensureInitialized(/* asRelroProducer= */ false,
                        Linker.PreferAddress.RESERVE_HINT, getLoadAddress());
            } else {
                // The main process expects the app zygote to provide the RELRO FD, but this process
                // does not inherit from the app zygote. This could be because:
                // 1. Running in a privileged process - very common
                // 2. Running in a renderer process - App Zygote was disabled due to opt out on
                //    low end devices - somewhat common
                // To cover both cases start with FIND_RESERVED, and proceed with fallbacks built
                // into the Linker initialization.
                //
                // TODO(pasko): Investigate whether searching with FIND_RESERVED affects startup
                // speed on Go devices.
                if (DEBUG) {
                    Log.i(TAG,
                            "initInChildProcess: child process not from app zygote, with address "
                                    + "hint: 0x%x",
                            getLoadAddress());
                }
                getLinker().ensureInitialized(/* asRelroProducer= */ false,
                        Linker.PreferAddress.FIND_RESERVED, getLoadAddress());
            }
            if (mCreatedIn != CreatedIn.ZYGOTE) mCreatedIn = CreatedIn.CHILD_WITHOUT_ZYGOTE;
            mInitDone = true;
        }

        /**
         * Optionally extracts RELRO and saves it for replacing the RELRO section in this process.
         * Can be invoked before initialization.
         * @param bundle Where to deserialize from.
         */
        public void takeSharedRelrosFromBundle(Bundle bundle) {
            if (useChromiumLinker()) {
                getLinker().takeSharedRelrosFromBundle(bundle);
            }
        }

        /**
         * Optionally puts the RELRO section information so that it can be memory-mapped in another
         * process reading the bundle.
         * @param bundle Where to serialize.
         */
        public void putSharedRelrosToBundle(Bundle bundle) {
            assert mInitDone;
            if (useChromiumLinker()) {
                getLinker().putSharedRelrosToBundle(bundle);
            }
        }

        private void recordLinkerHistogramsAfterLibraryLoad() {
            if (!useChromiumLinker()) return;
            // When recording a sample in the App Zygote it gets copied to each forked process and
            // hence gets duplicated in the uploads. Avoiding such duplication would require
            // serializing the samples, sending them to the browser process and disambiguating by,
            // for example, Zygote PID in ChildProcessConnection.java. A few rough performance
            // estimations do not require this complexity.
            getLinker().recordHistograms(creationAsString());
        }

        private String creationAsString() {
            switch (mCreatedIn) {
                case CreatedIn.MAIN:
                    return "Browser";
                case CreatedIn.ZYGOTE:
                    return "Zygote";
                case CreatedIn.CHILD_WITHOUT_ZYGOTE:
                    return "Child";
                default:
                    assert false : "Must initialize as one of {Browser,Zygote,Child}";
                    return "";
            }
        }

        private static final String LINKER_HISTOGRAM_PREFIX = "ChromiumAndroidLinker.";

        private void recordLoadTimeHistogram(long loadTimeMs) {
            RecordHistogram.recordTimesHistogram(
                    LINKER_HISTOGRAM_PREFIX + creationAsString() + "LoadTime2", loadTimeMs);
        }

        public void recordLoadThreadTimeHistogram(long threadLoadTimeMs) {
            RecordHistogram.recordTimesHistogram(
                    LINKER_HISTOGRAM_PREFIX + creationAsString() + "ThreadLoadTime",
                    threadLoadTimeMs);
        }
    }

    public final MultiProcessMediator getMediator() {
        return mMediator;
    }

    public static LibraryLoader getInstance() {
        return sInstance;
    }

    @VisibleForTesting
    protected LibraryLoader() {
        if (DEBUG) {
            logLinkerUsed();
        }
        if (BuildConfig.ENABLE_ASSERTS) {
            NativeLibraryLoadedStatus.setProvider(new NativeLibraryLoadedStatusProvider() {
                @Override
                public boolean areNativeMethodsReady() {
                    return isLoaded();
                }
            });
        }
    }

    /**
     * Set the {@link LibraryProcessType} for this process.
     *
     * Since this function is called extremely early on in startup, locking is not required.
     *
     * @param type the process type.
     */
    public void setLibraryProcessType(@LibraryProcessType int type) {
        assert type != LibraryProcessType.PROCESS_UNINITIALIZED;
        if (type == mLibraryProcessType) return;
        if (mLibraryProcessType != LibraryProcessType.PROCESS_UNINITIALIZED) {
            throw new IllegalStateException(
                    String.format("Trying to change the LibraryProcessType from %d to %d",
                            mLibraryProcessType, type));
        }
        mLibraryProcessType = type;
    }

    /**
     * Set native library preloader. If set and the Chromium linker is not used, the
     * {@link NativeLibraryPreloader#loadLibrary(String)} ()} will be invoked before calling
     * System.loadLibrary().
     *
     * @param loader the NativeLibraryPreloader, it shall only be set once and before the
     *               native library is loaded.
     */
    public void setNativeLibraryPreloader(NativeLibraryPreloader loader) {
        synchronized (mLock) {
            assert mLibraryPreloader == null;
            assert !mLoaded;
            mLibraryPreloader = loader;
        }
    }

    /**
     * Sets the configuration for library loading.
     *
     * Must be called before loading the library. Since this function is called extremely early on
     * in startup, locking is not required.
     *
     * @param useChromiumLinker Whether to use a chromium linker.
     */
    public void setLinkerImplementation(boolean useChromiumLinker) {
        assert !mInitialized;
        mUseChromiumLinker = useChromiumLinker;
        if (DEBUG) logLinkerUsed();
    }

    private void logLinkerUsed() {
        Log.i(TAG, "Configuration: useChromiumLinker() = %b", useChromiumLinker());
    }

    // Whether a Linker subclass replaces the system dynamic linker for loading. Even if returns
    // |true|, when the Linker fails, the system dynamic linker is used as a fallback. Also it is
    // common for App Zygote to choose loading with the system linker when sharing RELRO with the
    // browser process is not supported.
    private boolean useChromiumLinker() {
        return mUseChromiumLinker;
    }

    /**
     * Returns the singleton Linker instance.
     *
     * On N, O and P Monochrome is selected by Play Store. With Monochrome this code is not used,
     * instead Chrome asks the WebView to provide the library (and the shared RELRO). If the WebView
     * fails to provide the library, the system linker is used as a fallback.
     *
     * More: docs/android_native_libraries.md
     *
     * @return the Linker implementation instance.
     */
    private Linker getLinker() {
        assert useChromiumLinker();
        synchronized (mLock) {
            if (mLinker == null) mLinker = new Linker();
            return mLinker;
        }
    }

    /**
     * Return if library is already loaded successfully by the zygote.
     */
    public boolean isLoadedByZygote() {
        synchronized (mLock) {
            return mLoadedByZygote;
        }
    }

    /**
     *  Blocks until the library is fully loaded and initialized. When this method is used (without
     *  the {@link MultiProcessMediator}) the current process is treated as the Main process
     *  (w.r.t. how it shares RELRO and reports metrics) unless it was initialized before.
     */
    public void ensureInitialized() {
        if (isInitialized()) return;
        synchronized (mLock) {
            if (DEBUG) logLinkerUsed();
            loadAlreadyLocked(ContextUtils.getApplicationContext().getApplicationInfo(), false);
            initializeAlreadyLocked();
        }
    }

    /**
     * Calls native library preloader (see {@link #setNativeLibraryPreloader}) with the app
     * context. If there is no preloader set, this function does nothing.
     * Preloader is called only once, so calling it explicitly via this method means
     * that it won't be (implicitly) called during library loading.
     */
    public void preloadNow() {
        preloadNowOverridePackageName(
                ContextUtils.getApplicationContext().getApplicationInfo().packageName);
    }

    /**
     * Similar to {@link #preloadNow}, but allows specifying app context to use.
     */
    public void preloadNowOverridePackageName(String packageName) {
        synchronized (mLock) {
            if (useChromiumLinker()) return;
            preloadAlreadyLocked(packageName, /* inZygote= */ false);
        }
    }

    @GuardedBy("mLock")
    private void preloadAlreadyLocked(String packageName, boolean inZygote) {
        try (TraceEvent te = TraceEvent.scoped("LibraryLoader.preloadAlreadyLocked")) {
            // Preloader uses system linker, we shouldn't preload if Chromium linker is used.
            assert !useChromiumLinker() || (inZygote && mainProcessIntendsToProvideRelroFd());
            if (mLibraryPreloader != null && !mLibraryPreloaderCalled) {
                mLibraryPreloader.loadLibrary(packageName);
                mLibraryPreloaderCalled = true;
            }
        }
    }

    /**
     * Checks whether the native library is fully loaded.
     *
     * @deprecated: please avoid using in new code:
     * https://crsrc.org/c/base/android/jni_generator/README.md#testing-for-readiness-use-get
     */
    @Deprecated
    @VisibleForTesting
    public boolean isLoaded() {
        return mLoaded && (!sEnableStateForTesting || mLoadedForTesting);
    }

    /**
     * Checks whether the native library is fully loaded and initialized.
     *
     * @deprecated: please avoid using in new code:
     * https://chromium.googlesource.com/chromium/src/+/main/base/android/jni_generator/README.md#testing-for-readiness_use
     */
    @Deprecated
    public boolean isInitialized() {
        return mInitialized && isLoaded() && (!sEnableStateForTesting || mInitializedForTesting);
    }

    /**
     * Loads the library and blocks until the load completes. The caller is responsible for
     * subsequently calling ensureInitialized(). May be called on any thread, but should only be
     * called once.
     */
    public void loadNow() {
        loadNowOverrideApplicationContext(ContextUtils.getApplicationContext());
    }

    /**
     * Causes LibraryLoader to pretend that native libraries have not yet been initialized.
     */
    public void resetForTesting() {
        mLoadedForTesting = false;
        mInitializedForTesting = false;
        sEnableStateForTesting = true;
    }

    /**
     * Override kept for callers that need to load from a different app context. Do not use unless
     * specifically required to load from another context that is not the current process's app
     * context.
     *
     * @param appContext The overriding app context to be used to load libraries.
     */
    public void loadNowOverrideApplicationContext(Context appContext) {
        synchronized (mLock) {
            if (mLoaded && appContext != ContextUtils.getApplicationContext()) {
                throw new IllegalStateException("Attempt to load again from alternate context.");
            }
            loadAlreadyLocked(appContext.getApplicationInfo(), /* inZygote= */ false);
        }
    }

    public void loadNowInZygote(ApplicationInfo appInfo) {
        synchronized (mLock) {
            assert !mLoaded;
            loadAlreadyLocked(appInfo, /* inZygote= */ true);
            mLoadedByZygote = true;
        }
    }

    /**
     * Initializes the native library: must be called on the thread that the
     * native will call its "main" thread. The library must have previously been
     * loaded with one of the loadNow*() variants.
     */
    public void initialize() {
        synchronized (mLock) {
            initializeAlreadyLocked();
        }
    }

    /**
     * Enables the reached code profiler. The value comes from "ReachedCodeProfiler"
     * finch experiment, and is pushed on every run. I.e. the effect of the finch experiment
     * lags by one run, which is the best we can do considering that the profiler has to be enabled
     * before finch is initialized. Note that since LibraryLoader is in //base, it can't depend
     * on ChromeFeatureList, and has to rely on external code pushing the value.
     *
     * @param enabled whether to enable the reached code profiler.
     * @param samplingIntervalUs the sampling interval for reached code profiler.
     */
    public static void setReachedCodeProfilerEnabledOnNextRuns(
            boolean enabled, int samplingIntervalUs) {
        // Store 0 if the profiler is not enabled, otherwise store the sampling interval in
        // microseconds.
        if (enabled && samplingIntervalUs == 0) {
            samplingIntervalUs = DEFAULT_REACHED_CODE_SAMPLING_INTERVAL_US;
        } else if (!enabled) {
            samplingIntervalUs = 0;
        }
        SharedPreferences.Editor editor = ContextUtils.getAppSharedPreferences().edit();
        editor.remove(DEPRECATED_REACHED_CODE_PROFILER_KEY);
        editor.putInt(REACHED_CODE_SAMPLING_INTERVAL_KEY, samplingIntervalUs).apply();
    }

    /**
     * @return sampling interval for reached code profiler, or 0 when the profiler is disabled. (see
     *         setReachedCodeProfilerEnabledOnNextRuns()).
     */
    @VisibleForTesting
    public static int getReachedCodeSamplingIntervalUs() {
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            if (ContextUtils.getAppSharedPreferences().getBoolean(
                        DEPRECATED_REACHED_CODE_PROFILER_KEY, false)) {
                return DEFAULT_REACHED_CODE_SAMPLING_INTERVAL_US;
            }
            return ContextUtils.getAppSharedPreferences().getInt(
                    REACHED_CODE_SAMPLING_INTERVAL_KEY, 0);
        }
    }

    /**
     * Enables the background priority thread pool group. The value comes from the
     * "BackgroundThreadPool" finch experiment, and is pushed on every run, to take effect on the
     * subsequent run. I.e. the effect of the finch experiment lags by one run, which is the best we
     * can do considering that the thread pool has to be configured before finch is initialized.
     * Note that since LibraryLoader is in //base, it can't depend on ChromeFeatureList, and has to
     * rely on external code pushing the value.
     *
     * @param enabled whether to enable the background priority thread pool group.
     */
    public static void setBackgroundThreadPoolEnabledOnNextRuns(boolean enabled) {
        SharedPreferences.Editor editor = ContextUtils.getAppSharedPreferences().edit();
        editor.putBoolean(BACKGROUND_THREAD_POOL_KEY, enabled).apply();
    }

    /**
     * @return whether the background priority thread pool group should be enabled. (see
     *         setBackgroundThreadPoolEnabledOnNextRuns()).
     */
    @VisibleForTesting
    public static boolean isBackgroundThreadPoolEnabled() {
        try (StrictModeContext ignored = StrictModeContext.allowDiskReads()) {
            return ContextUtils.getAppSharedPreferences().getBoolean(
                    BACKGROUND_THREAD_POOL_KEY, false);
        }
    }

    private void loadWithChromiumLinker(ApplicationInfo appInfo, String library) {
        Linker linker = getLinker();
        String sourceDir = appInfo.sourceDir;
        Log.i(TAG, "Loading %s from within %s", library, sourceDir);
        linker.loadLibrary(library); // May throw UnsatisfiedLinkError.
        getMediator().recordLinkerHistogramsAfterLibraryLoad();
    }

    @GuardedBy("mLock")
    @SuppressLint({"UnsafeDynamicallyLoadedCode"})
    private void loadWithSystemLinkerAlreadyLocked(ApplicationInfo appInfo, boolean inZygote) {
        setEnvForNative();
        preloadAlreadyLocked(appInfo.packageName, inZygote);
        for (String library : NativeLibraries.LIBRARIES) {
            System.loadLibrary(library);
        }
    }

    // Invokes either Linker.loadLibrary(...), System.loadLibrary(...) or System.load(...),
    // triggering JNI_OnLoad in native code.
    @GuardedBy("mLock")
    @VisibleForTesting
    protected void loadAlreadyLocked(ApplicationInfo appInfo, boolean inZygote) {
        if (mLoaded) {
            if (sEnableStateForTesting && !mLoadedForTesting) {
                mLoadedForTesting = true;
            }
            return;
        }
        try (TraceEvent te = TraceEvent.scoped("LibraryLoader.loadAlreadyLocked")) {
            assert !mInitialized;
            assert mLibraryProcessType != LibraryProcessType.PROCESS_UNINITIALIZED || inZygote;

            UptimeMillisTimer uptimeTimer = new UptimeMillisTimer();
            CurrentThreadTimeMillisTimer threadTimeTimer = new CurrentThreadTimeMillisTimer();

            if (useChromiumLinker() && !mFallbackToSystemLinker) {
                if (DEBUG) Log.i(TAG, "Loading with the Chromium linker.");
                // See base/android/linker/config.gni, the chromium linker is only enabled when
                // we have a single library.
                assert NativeLibraries.LIBRARIES.length == 1;
                String library = NativeLibraries.LIBRARIES[0];
                loadWithChromiumLinker(appInfo, library);
            } else {
                if (DEBUG) Log.i(TAG, "Loading with the System linker.");
                loadWithSystemLinkerAlreadyLocked(appInfo, inZygote);
            }

            long loadTimeMs = uptimeTimer.getElapsedMillis();

            if (DEBUG) Log.i(TAG, "Time to load native libraries: %d ms", loadTimeMs);
            mLoaded = true;
            if (sEnableStateForTesting) {
                mLoadedForTesting = true;
            }

            getMediator().recordLoadTimeHistogram(loadTimeMs);
            getMediator().recordLoadThreadTimeHistogram(threadTimeTimer.getElapsedMillis());
        } catch (UnsatisfiedLinkError e) {
            throw new ProcessInitException(LoaderErrors.NATIVE_LIBRARY_LOAD_FAILED, e);
        }
    }

    // The WebView requires the Command Line to be switched over before
    // initialization is done. This is okay in the WebView's case since the
    // JNI is already loaded by this point.
    public void switchCommandLineForWebView() {
        synchronized (mLock) {
            ensureCommandLineSwitchedAlreadyLocked();
        }
    }

    // Switch the CommandLine over from Java to native if it hasn't already been done.
    // This must happen after the code is loaded and after JNI is ready (since after the
    // switch the Java CommandLine will delegate all calls the native CommandLine).
    @GuardedBy("mLock")
    private void ensureCommandLineSwitchedAlreadyLocked() {
        assert isLoaded();
        if (mCommandLineSwitched) {
            return;
        }
        CommandLine.enableNativeProxy();
        mCommandLineSwitched = true;
    }

    /**
     * Assert that library process type in the LibraryLoader is compatible with provided type.
     *
     * @param libraryProcessType a library process type to assert.
     */
    public void assertCompatibleProcessType(@LibraryProcessType int libraryProcessType) {
        assert libraryProcessType == mLibraryProcessType;
    }

    // Invoke base::android::LibraryLoaded in library_loader_hooks.cc
    @GuardedBy("mLock")
    private void initializeAlreadyLocked() {
        if (mInitialized) {
            if (sEnableStateForTesting) {
                mInitializedForTesting = true;
            }
            return;
        }
        assert mLibraryProcessType != LibraryProcessType.PROCESS_UNINITIALIZED;

        if (mLibraryProcessType == LibraryProcessType.PROCESS_BROWSER) {
            // Add a switch for the reached code profiler as late as possible since it requires a
            // read from the shared preferences. At this point the shared preferences are usually
            // warmed up.
            int reachedCodeSamplingIntervalUs = getReachedCodeSamplingIntervalUs();
            if (reachedCodeSamplingIntervalUs > 0) {
                CommandLine.getInstance().appendSwitch(BaseSwitches.ENABLE_REACHED_CODE_PROFILER);
                CommandLine.getInstance().appendSwitchWithValue(
                        BaseSwitches.REACHED_CODE_SAMPLING_INTERVAL_US,
                        Integer.toString(reachedCodeSamplingIntervalUs));
            }

            // Similarly, append a switch to enable the background thread pool group if the cached
            // preference indicates it should be enabled.
            if (isBackgroundThreadPoolEnabled()) {
                CommandLine.getInstance().appendSwitch(BaseSwitches.ENABLE_BACKGROUND_THREAD_POOL);
            }
        }

        ensureCommandLineSwitchedAlreadyLocked();

        if (!LibraryLoaderJni.get().libraryLoaded(mLibraryProcessType)) {
            Log.e(TAG, "error calling LibraryLoaderJni.get().libraryLoaded");
            throw new ProcessInitException(LoaderErrors.FAILED_TO_REGISTER_JNI);
        }

        // The "Successfully loaded native library" string is used by
        // tools/android/build_speed/benchmark.py. Please update that script if this log message is
        // changed.
        Log.i(TAG, "Successfully loaded native library");
        UmaRecorderHolder.onLibraryLoaded();

        // From now on, keep tracing in sync with native.
        TraceEvent.onNativeTracingReady();

        // From this point on, native code is ready to use. Note that this flag can be accessed
        // asynchronously, so any initialization must be performed before.
        mInitialized = true;
        if (sEnableStateForTesting) {
            mInitializedForTesting = true;
        }
    }

    /**
     * Overrides the library loader (normally with a mock) for testing.
     *
     * @deprecated: please avoid using in new code:
     * https://chromium.googlesource.com/chromium/src/+/main/base/android/jni_generator/README.md#testing-for-readiness_use
     *
     * @param loader the mock library loader.
     */
    @Deprecated
    @VisibleForTesting
    public static void setLibraryLoaderForTesting(LibraryLoader loader) {
        sInstance = loader;
    }

    /**
     * Configure ubsan using $UBSAN_OPTIONS. This function needs to be called before any native
     * libraries are loaded because ubsan reads its configuration from $UBSAN_OPTIONS when the
     * native library is loaded.
     */
    public static void setEnvForNative() {
        // The setenv API was added in L. On older versions of Android, we should still see ubsan
        // reports, but they will not have stack traces.
        if (BuildConfig.IS_UBSAN) {
            try {
                // This value is duplicated in build/android/pylib/constants/__init__.py.
                Os.setenv("UBSAN_OPTIONS",
                        "print_stacktrace=1 stack_trace_format='#%n pc %o %m' "
                                + "handle_segv=0 handle_sigbus=0 handle_sigfpe=0",
                        true);
            } catch (Exception e) {
                Log.w(TAG, "failed to set UBSAN_OPTIONS", e);
            }
        }
    }

    /**
     * This sets the LibraryLoader internal state to its fully initialized state and should *only*
     * be used by clients like NativeTests which manually load their native libraries without using
     * the LibraryLoader.
     *
     * Don't use in new code. Tests that require this call should be migrated to
     * NativeUnitTest.
     * https://chromium.googlesource.com/chromium/src/+/main/base/android/jni_generator/README.md#testing-for-readiness_use
     */
    protected static void setLibrariesLoadedForNativeTests() {
        LibraryLoader self = getInstance();
        self.mLoaded = true;
        self.mInitialized = true;
        if (sEnableStateForTesting) {
            self.mInitializedForTesting = true;
            self.mLoadedForTesting = true;
        }
    }

    public static void setBrowserProcessStartupBlockedForTesting() {
        sBrowserStartupBlockedForTesting = true;
    }

    public static boolean isBrowserProcessStartupBlockedForTesting() {
        return sBrowserStartupBlockedForTesting;
    }

    // The native methods below are defined in library_loader_hooks.cc.
    @NativeMethods
    interface Natives {
        // Performs auxiliary initialization useful right after the native library load. Returns
        // true on success and false on failure.
        boolean libraryLoaded(@LibraryProcessType int processType);
    }
}
