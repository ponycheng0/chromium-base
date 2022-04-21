// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_LOGGING_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_LOGGING_H_

#include <stddef.h>

#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>

#include "base/allocator/partition_allocator/partition_alloc_base/migration_adapter.h"
#include "base/allocator/partition_allocator/partition_alloc_base/scoped_clear_last_error.h"
#include "base/base_export.h"
#include "base/compiler_specific.h"
#include "base/dcheck_is_on.h"
#include "build/build_config.h"

// TODO(1151236): Need to update the description, because logging for PA
// standalone library was minimized.
//
// Optional message capabilities
// -----------------------------
// Assertion failed messages and fatal errors are displayed in a dialog box
// before the application exits. However, running this UI creates a message
// loop, which causes application messages to be processed and potentially
// dispatched to existing application windows. Since the application is in a
// bad state when this assertion dialog is displayed, these messages may not
// get processed and hang the dialog, or the application might go crazy.
//
// Therefore, it can be beneficial to display the error dialog in a separate
// process from the main application. When the logging system needs to display
// a fatal error dialog box, it will look for a program called
// "DebugMessage.exe" in the same directory as the application executable. It
// will run this application with the message as the command line, and will
// not include the name of the application as is traditional for easier
// parsing.
//
// The code for DebugMessage.exe is only one line. In WinMain, do:
//   MessageBox(NULL, GetCommandLineW(), L"Fatal Error", 0);
//
// If DebugMessage.exe is not found, the logging code will use a normal
// MessageBox, potentially causing the problems discussed above.

// Instructions
// ------------
//
// Make a bunch of macros for logging.  The way to log things is to stream
// things to PA_LOG(<a particular severity level>).  E.g.,
//
//   PA_LOG(INFO) << "Found " << num_cookies << " cookies";
//
// You can also do conditional logging:
//
//   PA_LOG_IF(INFO, num_cookies > 10) << "Got lots of cookies";
//
// The CHECK(condition) macro is active in both debug and release builds and
// effectively performs a PA_LOG(FATAL) which terminates the process and
// generates a crashdump unless a debugger is attached.
//
// There are also "debug mode" logging macros like the ones above:
//
//   PA_DLOG(INFO) << "Found cookies";
//
//   PA_DLOG_IF(INFO, num_cookies > 10) << "Got lots of cookies";
//
// All "debug mode" logging is compiled away to nothing for non-debug mode
// compiles.  PA_LOG_IF and development flags also work well together
// because the code can be compiled away sometimes.
//
// We also have
//
//   PA_LOG_ASSERT(assertion);
//   PA_DLOG_ASSERT(assertion);
//
// which is syntactic sugar for PA_{,D}LOG_IF(FATAL, assert fails) << assertion;
//
// There are "verbose level" logging macros.  They look like
//
//   PA_VLOG(1) << "I'm printed when you run the program with --v=1 or more";
//   PA_VLOG(2) << "I'm printed when you run the program with --v=2 or more";
//
// These always log at the INFO log level (when they log at all).
//
// There's also PA_VLOG_IS_ON(n) "verbose level" condition macro. To be used as
//
//   if (PA_VLOG_IS_ON(2)) {
//     // do some logging preparation and logging
//     // that can't be accomplished with just PA_VLOG(2) << ...;
//   }
//
// There is also a PA_VLOG_IF "verbose level" condition macro for sample
// cases, when some extra computation and preparation for logs is not
// needed.
//
//   PA_VLOG_IF(1, (size > 1024))
//      << "I'm printed when size is more than 1024 and when you run the "
//         "program with --v=1 or more";
//
// We also override the standard 'assert' to use 'PA_DLOG_ASSERT'.
//
// Lastly, there is:
//
//   PA_PLOG(ERROR) << "Couldn't do foo";
//   PA_DPLOG(ERROR) << "Couldn't do foo";
//   PA_PLOG_IF(ERROR, cond) << "Couldn't do foo";
//   PA_DPLOG_IF(ERROR, cond) << "Couldn't do foo";
//   PA_PCHECK(condition) << "Couldn't do foo";
//   PA_DPCHECK(condition) << "Couldn't do foo";
//
// which append the last system error to the message in string form (taken from
// GetLastError() on Windows and errno on POSIX).
//
// The supported severity levels for macros that allow you to specify one
// are (in increasing order of severity) INFO, WARNING, ERROR, and FATAL.
//
// Very important: logging a message at the FATAL severity level causes
// the program to terminate (after the message is logged).
//
// There is the special severity of DFATAL, which logs FATAL in DCHECK-enabled
// builds, ERROR in normal mode.
//
// Output is formatted as per the following example:
// [VERBOSE1:drm_device_handle.cc(90)] Succeeded
// authenticating /dev/dri/card0 in 0 ms with 1 attempt(s)
//
// The colon separated fields inside the brackets are as follows:
// 1. The log level
// 2. The filename and line number where the log was instantiated
//
// Additional logging-related information can be found here:
// https://chromium.googlesource.com/chromium/src/+/main/docs/linux/debugging.md#Logging

namespace partition_alloc::internal::logging {

// Sets the log level. Anything at or above this level will be written to the
// log file/displayed to the user (if applicable). Anything below this level
// will be silently ignored. The log level defaults to 0 (everything is logged
// up to level INFO) if this function is not called.
// Note that log messages for VLOG(x) are logged at level -x, so setting
// the min log level to negative values enables verbose logging.
BASE_EXPORT void SetMinLogLevel(int level);

// Gets the current log level.
BASE_EXPORT int GetMinLogLevel();

// Used by PA_LOG_IS_ON to lazy-evaluate stream arguments.
BASE_EXPORT bool ShouldCreateLogMessage(int severity);

// Gets the PA_VLOG default verbosity level.
BASE_EXPORT int GetVlogVerbosity();

// Sets the Log Message Handler that gets passed every log message before
// it's sent to other log destinations (if any).
// Returns true to signal that it handled the message and the message
// should not be sent to other log destinations.
typedef bool (*LogMessageHandlerFunction)(int severity,
                                          const char* file,
                                          int line,
                                          size_t message_start,
                                          const std::string& str);
BASE_EXPORT void SetLogMessageHandler(LogMessageHandlerFunction handler);
BASE_EXPORT LogMessageHandlerFunction GetLogMessageHandler();

using LogSeverity = int;
constexpr LogSeverity LOGGING_VERBOSE = -1;  // This is level 1 verbosity
// Note: the log severities are used to index into the array of names,
// see log_severity_names.
constexpr LogSeverity LOGGING_INFO = 0;
constexpr LogSeverity LOGGING_WARNING = 1;
constexpr LogSeverity LOGGING_ERROR = 2;
constexpr LogSeverity LOGGING_FATAL = 3;
constexpr LogSeverity LOGGING_NUM_SEVERITIES = 4;

// LOGGING_DFATAL is LOGGING_FATAL in DCHECK-enabled builds, ERROR in normal
// mode.
#if DCHECK_IS_ON()
constexpr LogSeverity LOGGING_DFATAL = LOGGING_FATAL;
#else
constexpr LogSeverity LOGGING_DFATAL = LOGGING_ERROR;
#endif

// This block duplicates the above entries to facilitate incremental conversion
// from LOG_FOO to LOGGING_FOO.
// TODO(thestig): Convert existing users to LOGGING_FOO and remove this block.
constexpr LogSeverity LOG_VERBOSE = LOGGING_VERBOSE;
constexpr LogSeverity LOG_INFO = LOGGING_INFO;
constexpr LogSeverity LOG_WARNING = LOGGING_WARNING;
constexpr LogSeverity LOG_ERROR = LOGGING_ERROR;
constexpr LogSeverity LOG_FATAL = LOGGING_FATAL;
constexpr LogSeverity LOG_DFATAL = LOGGING_DFATAL;

// A few definitions of macros that don't generate much code. These are used
// by PA_LOG() and LOG_IF, etc. Since these are used all over our code, it's
// better to have compact code for these operations.
#define PA_COMPACT_GOOGLE_LOG_EX_INFO(ClassName, ...)                         \
  ::partition_alloc::internal::logging::ClassName(                            \
      __FILE__, __LINE__, ::partition_alloc::internal::logging::LOGGING_INFO, \
      ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_EX_WARNING(ClassName, ...) \
  ::partition_alloc::internal::logging::ClassName(       \
      __FILE__, __LINE__,                                \
      ::partition_alloc::internal::logging::LOGGING_WARNING, ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_EX_ERROR(ClassName, ...)                         \
  ::partition_alloc::internal::logging::ClassName(                             \
      __FILE__, __LINE__, ::partition_alloc::internal::logging::LOGGING_ERROR, \
      ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_EX_FATAL(ClassName, ...)                         \
  ::partition_alloc::internal::logging::ClassName(                             \
      __FILE__, __LINE__, ::partition_alloc::internal::logging::LOGGING_FATAL, \
      ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_EX_DFATAL(ClassName, ...) \
  ::partition_alloc::internal::logging::ClassName(      \
      __FILE__, __LINE__,                               \
      ::partition_alloc::internal::logging::LOGGING_DFATAL, ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_EX_DCHECK(ClassName, ...) \
  ::partition_alloc::internal::logging::ClassName(      \
      __FILE__, __LINE__,                               \
      ::partition_alloc::internal::logging::LOGGING_DCHECK, ##__VA_ARGS__)

#define PA_COMPACT_GOOGLE_LOG_INFO PA_COMPACT_GOOGLE_LOG_EX_INFO(LogMessage)
#define PA_COMPACT_GOOGLE_LOG_WARNING \
  PA_COMPACT_GOOGLE_LOG_EX_WARNING(LogMessage)
#define PA_COMPACT_GOOGLE_LOG_ERROR PA_COMPACT_GOOGLE_LOG_EX_ERROR(LogMessage)
#define PA_COMPACT_GOOGLE_LOG_FATAL PA_COMPACT_GOOGLE_LOG_EX_FATAL(LogMessage)
#define PA_COMPACT_GOOGLE_LOG_DFATAL PA_COMPACT_GOOGLE_LOG_EX_DFATAL(LogMessage)
#define PA_COMPACT_GOOGLE_LOG_DCHECK PA_COMPACT_GOOGLE_LOG_EX_DCHECK(LogMessage)

#if BUILDFLAG(IS_WIN)
// wingdi.h defines ERROR to be 0. When we call PA_LOG(ERROR), it gets
// substituted with 0, and it expands to PA_COMPACT_GOOGLE_LOG_0. To allow us
// to keep using this syntax, we define this macro to do the same thing
// as PA_COMPACT_GOOGLE_LOG_ERROR, and also define ERROR the same way that
// the Windows SDK does for consistency.
#define PA_ERROR 0
#define PA_COMPACT_GOOGLE_LOG_EX_0(ClassName, ...) \
  PA_COMPACT_GOOGLE_LOG_EX_ERROR(ClassName, ##__VA_ARGS__)
#define PA_COMPACT_GOOGLE_LOG_0 PA_COMPACT_GOOGLE_LOG_ERROR
// Needed for LOG_IS_ON(ERROR).
constexpr LogSeverity LOGGING_0 = LOGGING_ERROR;
#endif

// As special cases, we can assume that LOG_IS_ON(FATAL) always holds. Also,
// LOG_IS_ON(DFATAL) always holds in debug mode. In particular, CHECK()s will
// always fire if they fail.
#define PA_LOG_IS_ON(severity)                                   \
  (::partition_alloc::internal::logging::ShouldCreateLogMessage( \
      ::partition_alloc::internal::logging::LOGGING_##severity))

// We don't do any caching tricks with VLOG_IS_ON() like the
// google-glog version since it increases binary size.  This means
// that using the v-logging functions in conjunction with --vmodule
// may be slow.
#define PA_VLOG_IS_ON(verboselevel) \
  ((verboselevel) <= ::partition_alloc::internal::logging::GetVlogVerbosity())

// Helper macro which avoids evaluating the arguments to a stream if
// the condition doesn't hold. Condition is evaluated once and only once.
#define PA_LAZY_STREAM(stream, condition) \
  !(condition)                            \
      ? (void)0                           \
      : ::partition_alloc::internal::logging::LogMessageVoidify() & (stream)

// We use the preprocessor's merging operator, "##", so that, e.g.,
// PA_LOG(INFO) becomes the token PA_COMPACT_GOOGLE_LOG_INFO.  There's some
// funny subtle difference between ostream member streaming functions (e.g.,
// ostream::operator<<(int) and ostream non-member streaming functions
// (e.g., ::operator<<(ostream&, string&): it turns out that it's
// impossible to stream something like a string directly to an unnamed
// ostream. We employ a neat hack by calling the stream() member
// function of LogMessage which seems to avoid the problem.
#define PA_LOG_STREAM(severity) PA_COMPACT_GOOGLE_LOG_##severity.stream()

#define PA_LOG(severity) \
  PA_LAZY_STREAM(PA_LOG_STREAM(severity), PA_LOG_IS_ON(severity))
#define PA_LOG_IF(severity, condition) \
  PA_LAZY_STREAM(PA_LOG_STREAM(severity), PA_LOG_IS_ON(severity) && (condition))

// The VLOG macros log with negative verbosities.
#define PA_VLOG_STREAM(verbose_level)                                  \
  ::partition_alloc::internal::logging::LogMessage(__FILE__, __LINE__, \
                                                   -(verbose_level))   \
      .stream()

#define PA_VLOG(verbose_level) \
  PA_LAZY_STREAM(PA_VLOG_STREAM(verbose_level), PA_VLOG_IS_ON(verbose_level))

#define PA_VLOG_IF(verbose_level, condition)    \
  PA_LAZY_STREAM(PA_VLOG_STREAM(verbose_level), \
                 PA_VLOG_IS_ON(verbose_level) && (condition))

#if BUILDFLAG(IS_WIN)
#define PA_VPLOG_STREAM(verbose_level)                                \
  ::partition_alloc::internal::logging::Win32ErrorLogMessage(         \
      __FILE__, __LINE__, -(verbose_level),                           \
      ::partition_alloc::internal::logging::GetLastSystemErrorCode()) \
      .stream()
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#define PA_VPLOG_STREAM(verbose_level)                                \
  ::partition_alloc::internal::logging::ErrnoLogMessage(              \
      __FILE__, __LINE__, -(verbose_level),                           \
      ::partition_alloc::internal::logging::GetLastSystemErrorCode()) \
      .stream()
#endif

#define PA_VPLOG(verbose_level) \
  PA_LAZY_STREAM(PA_VPLOG_STREAM(verbose_level), PA_VLOG_IS_ON(verbose_level))

#define PA_VPLOG_IF(verbose_level, condition)    \
  PA_LAZY_STREAM(PA_VPLOG_STREAM(verbose_level), \
                 PA_VLOG_IS_ON(verbose_level) && (condition))

// TODO(akalin): Add more VLOG variants, e.g. VPLOG.

#define PA_LOG_ASSERT(condition)                       \
  PA_LOG_IF(FATAL, !(ANALYZER_ASSUME_TRUE(condition))) \
      << "Assert failed: " #condition ". "

#if BUILDFLAG(IS_WIN)
#define PA_PLOG_STREAM(severity)                                      \
  PA_COMPACT_GOOGLE_LOG_EX_##severity(                                \
      Win32ErrorLogMessage,                                           \
      ::partition_alloc::internal::logging::GetLastSystemErrorCode()) \
      .stream()
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#define PA_PLOG_STREAM(severity)                                      \
  PA_COMPACT_GOOGLE_LOG_EX_##severity(                                \
      ErrnoLogMessage,                                                \
      ::partition_alloc::internal::logging::GetLastSystemErrorCode()) \
      .stream()
#endif

#define PA_PLOG(severity) \
  PA_LAZY_STREAM(PA_PLOG_STREAM(severity), PA_LOG_IS_ON(severity))

#define PA_PLOG_IF(severity, condition)    \
  PA_LAZY_STREAM(PA_PLOG_STREAM(severity), \
                 PA_LOG_IS_ON(severity) && (condition))

BASE_EXPORT extern std::ostream* g_swallow_stream;

// Note that g_swallow_stream is used instead of an arbitrary PA_LOG() stream to
// avoid the creation of an object with a non-trivial destructor (LogMessage).
// On MSVC x86 (checked on 2015 Update 3), this causes a few additional
// pointless instructions to be emitted even at full optimization level, even
// though the : arm of the ternary operator is clearly never executed. Using a
// simpler object to be &'d with Voidify() avoids these extra instructions.
// Using a simpler POD object with a templated operator<< also works to avoid
// these instructions. However, this causes warnings on statically defined
// implementations of operator<<(std::ostream, ...) in some .cc files, because
// they become defined-but-unreferenced functions. A reinterpret_cast of 0 to an
// ostream* also is not suitable, because some compilers warn of undefined
// behavior.
#define PA_EAT_STREAM_PARAMETERS                                     \
  true ? (void)0                                                     \
       : ::partition_alloc::internal::logging::LogMessageVoidify() & \
             (*::partition_alloc::internal::logging::g_swallow_stream)

// Definitions for DLOG et al.

#if DCHECK_IS_ON()

#define PA_DLOG_IS_ON(severity) PA_LOG_IS_ON(severity)
#define PA_DLOG_IF(severity, condition) PA_LOG_IF(severity, condition)
#define PA_DLOG_ASSERT(condition) PA_LOG_ASSERT(condition)
#define PA_DPLOG_IF(severity, condition) PA_PLOG_IF(severity, condition)
#define PA_DVLOG_IF(verboselevel, condition) PA_VLOG_IF(verboselevel, condition)
#define PA_DVPLOG_IF(verboselevel, condition) \
  PA_VPLOG_IF(verboselevel, condition)

#else  // DCHECK_IS_ON()

// If !DCHECK_IS_ON(), we want to avoid emitting any references to |condition|
// (which may reference a variable defined only if DCHECK_IS_ON()).
// Contrast this with DCHECK et al., which has different behavior.

#define PA_DLOG_IS_ON(severity) false
#define PA_DLOG_IF(severity, condition) PA_EAT_STREAM_PARAMETERS
#define PA_DLOG_ASSERT(condition) PA_EAT_STREAM_PARAMETERS
#define PA_DPLOG_IF(severity, condition) PA_EAT_STREAM_PARAMETERS
#define PA_DVLOG_IF(verboselevel, condition) PA_EAT_STREAM_PARAMETERS
#define PA_DVPLOG_IF(verboselevel, condition) PA_EAT_STREAM_PARAMETERS

#endif  // DCHECK_IS_ON()

#define PA_DLOG(severity) \
  PA_LAZY_STREAM(PA_LOG_STREAM(severity), PA_DLOG_IS_ON(severity))

#define PA_DPLOG(severity) \
  PA_LAZY_STREAM(PA_PLOG_STREAM(severity), PA_DLOG_IS_ON(severity))

#define PA_DVLOG(verboselevel) PA_DVLOG_IF(verboselevel, true)

#define PA_DVPLOG(verboselevel) PA_DVPLOG_IF(verboselevel, true)

// Definitions for DCHECK et al.

#if defined(DCHECK_IS_CONFIGURABLE)
BASE_EXPORT extern LogSeverity LOGGING_DCHECK;
#else
constexpr LogSeverity LOGGING_DCHECK = LOGGING_FATAL;
#endif  // defined(DCHECK_IS_CONFIGURABLE)

// Redefine the standard assert to use our nice log files
#undef assert
#define assert(x) PA_DLOG_ASSERT(x)

// This class more or less represents a particular log message.  You
// create an instance of LogMessage and then stream stuff to it.
// When you finish streaming to it, ~LogMessage is called and the
// full message gets streamed to the appropriate destination.
//
// You shouldn't actually use LogMessage's constructor to log things,
// though.  You should use the PA_LOG() macro (and variants thereof)
// above.
class BASE_EXPORT LogMessage {
 public:
  // Used for PA_LOG(severity).
  LogMessage(const char* file, int line, LogSeverity severity);

  // Used for CHECK().  Implied severity = LOGGING_FATAL.
  LogMessage(const char* file, int line, const char* condition);
  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;
  virtual ~LogMessage();

  std::ostream& stream() { return stream_; }

  LogSeverity severity() { return severity_; }
  std::string str() { return stream_.str(); }

 private:
  void Init(const char* file, int line);

  const LogSeverity severity_;
  std::ostringstream stream_;
  size_t message_start_;  // Offset of the start of the message (past prefix
                          // info).
  // The file and line information passed in to the constructor.
  const char* const file_;
  const int line_;

  // This is useful since the LogMessage class uses a lot of Win32 calls
  // that will lose the value of GLE and the code that called the log function
  // will have lost the thread error value when the log call returns.
  base::ScopedClearLastError last_error_;
};

// This class is used to explicitly ignore values in the conditional
// logging macros.  This avoids compiler warnings like "value computed
// is not used" and "statement has no effect".
class LogMessageVoidify {
 public:
  LogMessageVoidify() = default;
  // This has to be an operator with a precedence lower than << but
  // higher than ?:
  void operator&(std::ostream&) {}
};

#if BUILDFLAG(IS_WIN)
typedef unsigned long SystemErrorCode;
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
typedef int SystemErrorCode;
#endif

// Alias for ::GetLastError() on Windows and errno on POSIX. Avoids having to
// pull in windows.h just for GetLastError() and DWORD.
BASE_EXPORT SystemErrorCode GetLastSystemErrorCode();
BASE_EXPORT std::string SystemErrorCodeToString(SystemErrorCode error_code);

#if BUILDFLAG(IS_WIN)
// Appends a formatted system message of the GetLastError() type.
class BASE_EXPORT Win32ErrorLogMessage : public LogMessage {
 public:
  Win32ErrorLogMessage(const char* file,
                       int line,
                       LogSeverity severity,
                       SystemErrorCode err);
  Win32ErrorLogMessage(const Win32ErrorLogMessage&) = delete;
  Win32ErrorLogMessage& operator=(const Win32ErrorLogMessage&) = delete;
  // Appends the error message before destructing the encapsulated class.
  ~Win32ErrorLogMessage() override;

 private:
  SystemErrorCode err_;
};
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
// Appends a formatted system message of the errno type
class BASE_EXPORT ErrnoLogMessage : public LogMessage {
 public:
  ErrnoLogMessage(const char* file,
                  int line,
                  LogSeverity severity,
                  SystemErrorCode err);
  ErrnoLogMessage(const ErrnoLogMessage&) = delete;
  ErrnoLogMessage& operator=(const ErrnoLogMessage&) = delete;
  // Appends the error message before destructing the encapsulated class.
  ~ErrnoLogMessage() override;

 private:
  SystemErrorCode err_;
};
#endif  // BUILDFLAG(IS_WIN)

// Async signal safe logging mechanism.
BASE_EXPORT void RawLog(int level, const char* message);

#define PA_RAW_LOG(level, message)              \
  ::partition_alloc::internal::logging::RawLog( \
      ::partition_alloc::internal::logging::LOGGING_##level, message)

}  // namespace partition_alloc::internal::logging

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_LOGGING_H_
