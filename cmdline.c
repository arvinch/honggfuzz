/*

   honggfuzz - cmdline parsing

   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include "cmdline.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#if defined(_HF_ARCH_LINUX)
#include <sched.h>
#endif /* defined(_HF_ARCH_LINUX) */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "display.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"

struct custom_option {
    struct option opt;
    const char* descr;
};

static bool checkFor_FILE_PLACEHOLDER(const char* const* args) {
    for (int x = 0; args[x]; x++) {
        if (strstr(args[x], _HF_FILE_PLACEHOLDER)) {
            return true;
        }
    }
    return false;
}

static bool cmdlineCheckBinaryType(honggfuzz_t* hfuzz) {
    int fd;
    off_t fileSz;
    uint8_t* map = files_mapFile(hfuzz->exe.cmdline[0], &fileSz, &fd, /* isWriteable= */ false);
    if (!map) {
        /* It's not a critical error */
        return true;
    }
    defer {
        if (munmap(map, fileSz) == -1) {
            PLOG_W("munmap(%p, %zu)", map, (size_t)fileSz);
        }
        close(fd);
    };

    if (memmem(map, fileSz, _HF_PERSISTENT_SIG, strlen(_HF_PERSISTENT_SIG))) {
        LOG_I("Persistent signature found in '%s'. Enabling persistent fuzzing mode",
            hfuzz->exe.cmdline[0]);
        hfuzz->exe.persistent = true;
    }
    if (memmem(map, fileSz, _HF_NETDRIVER_SIG, strlen(_HF_NETDRIVER_SIG))) {
        LOG_I("NetDriver signature found '%s'", hfuzz->exe.cmdline[0]);
        hfuzz->exe.netDriver = true;
    }
    return true;
}

static const char* cmdlineYesNo(bool yes) {
    return (yes ? "true" : "false");
}

static void cmdlineHelp(const char* pname, struct custom_option* opts) {
    LOG_HELP_BOLD("Usage: %s [options] -- path_to_command [args]", pname);
    LOG_HELP_BOLD("Options:");
    for (int i = 0; opts[i].opt.name; i++) {
        if (isprint(opts[i].opt.val) && opts[i].opt.val < 0x80) {
            LOG_HELP_BOLD(" --%s%s%c %s", opts[i].opt.name, "|-", opts[i].opt.val,
                opts[i].opt.has_arg == required_argument ? "VALUE" : "");
        } else {
            LOG_HELP_BOLD(" --%s %s", opts[i].opt.name,
                opts[i].opt.has_arg == required_argument ? "VALUE" : "");
        }
        LOG_HELP("\t%s", opts[i].descr);
    }
    LOG_HELP_BOLD("\nExamples:");
    LOG_HELP(
        " Run the binary over a mutated file chosen from the directory. Disable fuzzing feedback "
        "(static mode):");
    LOG_HELP_BOLD("  " PROG_NAME " -i input_dir -x -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
    LOG_HELP(" As above, provide input over STDIN:");
    LOG_HELP_BOLD("  " PROG_NAME " -i input_dir -x -s -- /usr/bin/djpeg");
    LOG_HELP(" Use compile-time instrumentation (-fsanitize-coverage=trace-pc-guard,...):");
    LOG_HELP_BOLD("  " PROG_NAME " -i input_dir -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
    LOG_HELP(" Use persistent mode w/o instrumentation:");
    LOG_HELP_BOLD("  " PROG_NAME " -i input_dir -P -x -- /usr/bin/djpeg_persistent_mode");
    LOG_HELP(" Use persistent mode and compile-time (-fsanitize-coverage=trace-pc-guard,...) "
             "instrumentation:");
    LOG_HELP_BOLD("  " PROG_NAME " -i input_dir -P -- /usr/bin/djpeg_persistent_mode");
#if defined(_HF_ARCH_LINUX)
    LOG_HELP(
        " Run the binary with dynamically generate inputs, maximize total no. of instructions:");
    LOG_HELP_BOLD("  " PROG_NAME " --linux_perf_instr -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
    LOG_HELP(" As above, maximize total no. of branches:");
    LOG_HELP_BOLD("  " PROG_NAME " --linux_perf_branch -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
    LOG_HELP(" As above, maximize unique branches (edges) via Intel BTS:");
    LOG_HELP_BOLD("  " PROG_NAME " --linux_perf_bts_edge -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
    LOG_HELP(
        " As above, maximize unique code blocks via Intel Processor Trace (requires libipt.so):");
    LOG_HELP_BOLD("  " PROG_NAME " --linux_perf_ipt_block -- /usr/bin/djpeg " _HF_FILE_PLACEHOLDER);
#endif /* defined(_HF_ARCH_LINUX) */
}

static void cmdlineUsage(const char* pname, struct custom_option* opts) {
    cmdlineHelp(pname, opts);
    exit(0);
}

bool cmdlineAddEnv(honggfuzz_t* hfuzz, char* env) {
    size_t enveqlen = strlen(env);
    const char* eqpos = strchr(env, '=');
    if (eqpos) {
        enveqlen = (uintptr_t)eqpos - (uintptr_t)env + 1;
    }

    for (size_t i = 0; i < ARRAYSIZE(hfuzz->exe.env_ptrs); i++) {
        if (hfuzz->exe.env_ptrs[i] == NULL) {
            LOG_D("Adding envar '%s' at pos: %zu", env, i);
            hfuzz->exe.env_ptrs[i] = hfuzz->exe.env_vals[i];
            snprintf(hfuzz->exe.env_vals[i], sizeof(hfuzz->exe.env_vals[i]), "%s", env);
            return true;
        }
        if (strncmp(hfuzz->exe.env_vals[i], env, enveqlen) == 0) {
            LOG_W("Replacing envar '%s' with '%s'", hfuzz->exe.env_vals[i], env);
            snprintf(hfuzz->exe.env_vals[i], sizeof(hfuzz->exe.env_vals[i]), "%s", env);
            hfuzz->exe.env_ptrs[i] = hfuzz->exe.env_vals[i];
            return true;
        }
    }
    LOG_E("No more space for new envars (max.%zu)", ARRAYSIZE(hfuzz->exe.env_ptrs));
    return false;
}

bool cmdlineParseTrueFalse(const char* optname, const char* optarg) {
    if (!optarg) {
        LOG_F("Option '--%s' needs an argument (true|false)", optname);
    }
    /* Probably '-' belong to the next option */
    if (optarg[0] == '-') {
        LOG_F("Option '--%s' needs an argument (true|false)", optname);
    }
    if ((strcasecmp(optarg, "0") == 0) || (strcasecmp(optarg, "false") == 0) ||
        (strcasecmp(optarg, "n") == 0) || (strcasecmp(optarg, "no") == 0)) {
        return false;
    }
    if ((strcasecmp(optarg, "1") == 0) || (strcasecmp(optarg, "true") == 0) ||
        (strcasecmp(optarg, "y") == 0) || (strcasecmp(optarg, "yes") == 0)) {
        return true;
    }
    LOG_F("Unknown value for option --%s=%s. Use true or false", optname, optarg);
    return false;
}

rlim_t cmdlineParseRLimit(int res, const char* optarg, unsigned long mul) {
    struct rlimit cur;
    if (getrlimit(res, &cur) == -1) {
        PLOG_F("getrlimit(%d)", res);
    }
    if (strcasecmp(optarg, "max") == 0) {
        return cur.rlim_max;
    }
    if (strcasecmp(optarg, "def") == 0) {
        return cur.rlim_cur;
    }
    if (util_isANumber(optarg) == false) {
        LOG_F("RLIMIT %d needs a numeric or 'max'/'def' value ('%s' provided)", res, optarg);
    }
    rlim_t val = strtoul(optarg, NULL, 0) * mul;
    if ((unsigned long)val == ULONG_MAX && errno != 0) {
        PLOG_F("strtoul('%s', 0)", optarg);
    }
    return val;
}

static bool cmdlineVerify(honggfuzz_t* hfuzz) {
    if (!cmdlineCheckBinaryType(hfuzz)) {
        LOG_E("Couldn't test binary for signatures");
        return false;
    }

    if (!hfuzz->exe.fuzzStdin && !hfuzz->exe.persistent &&
        !checkFor_FILE_PLACEHOLDER(hfuzz->exe.cmdline)) {
        LOG_E("You must specify '" _HF_FILE_PLACEHOLDER
              "' if the -s (stdin fuzzing) or --persistent options are not set");
        return false;
    }

    if (hfuzz->threads.threadsMax >= _HF_THREAD_MAX) {
        LOG_E("Too many fuzzing threads specified %zu (>= _HF_THREAD_MAX (%u))",
            hfuzz->threads.threadsMax, _HF_THREAD_MAX);
        return false;
    }
    if (hfuzz->threads.threadsMax == 0) {
        LOG_E("Too few fuzzing threads specified: %zu)", hfuzz->threads.threadsMax);
        return false;
    }

    if (strchr(hfuzz->io.fileExtn, '/')) {
        LOG_E("The file extension contains the '/' character: '%s'", hfuzz->io.fileExtn);
        return false;
    }

    if (strlen(hfuzz->io.workDir) == 0) {
        if (getcwd(hfuzz->io.workDir, sizeof(hfuzz->io.workDir)) == NULL) {
            PLOG_W("getcwd() failed. Using '.'");
            snprintf(hfuzz->io.workDir, sizeof(hfuzz->io.workDir), ".");
        }
    }
    if (mkdir(hfuzz->io.workDir, 0700) == -1 && errno != EEXIST) {
        PLOG_E("Couldn't create the workspace directory '%s'", hfuzz->io.workDir);
        return false;
    }
    if (hfuzz->io.crashDir == NULL) {
        hfuzz->io.crashDir = hfuzz->io.workDir;
    }
    if (mkdir(hfuzz->io.crashDir, 0700) && errno != EEXIST) {
        PLOG_E("Couldn't create the crash directory '%s'", hfuzz->io.crashDir);
        return false;
    }

    if (hfuzz->mutate.mutationsPerRun == 0U && hfuzz->cfg.useVerifier) {
        LOG_I("Verifier enabled with mutationsPerRun == 0, activating the dry run mode");
    }

    if (hfuzz->mutate.maxFileSz > _HF_INPUT_MAX_SIZE) {
        LOG_E("Maximum file size '%zu' bigger than the maximum size '%zu'", hfuzz->mutate.maxFileSz,
            (size_t)_HF_INPUT_MAX_SIZE);
        return false;
    }

    return true;
}

bool cmdlineParse(int argc, char* argv[], honggfuzz_t* hfuzz) {
    *hfuzz = (honggfuzz_t){
        .threads =
            {
                .threadsFinished = 0,
                .threadsMax = ({
                    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
                    (ncpus <= 1 ? 1 : ncpus / 2);
                }),
                .threadsActiveCnt = 0,
                .mainThread = pthread_self(),
                .mainPid = getpid(),
            },
        .io =
            {
                .inputDir = NULL,
                .outputDir = NULL,
                .inputDirPtr = NULL,
                .fileCnt = 0,
                .fileCntDone = false,
                .newUnitsAdded = 0,
                .fileExtn = "fuzz",
                .workDir = {},
                .crashDir = NULL,
                .covDirNew = NULL,
                .saveUnique = true,
                .dynfileqCnt = 0U,
                .dynfileq_mutex = PTHREAD_RWLOCK_INITIALIZER,
                .dynfileqCurrent = NULL,
                .exportFeedback = false,
            },
        .exe =
            {
                .argc = 0,
                .cmdline = NULL,
                .nullifyStdio = true,
                .fuzzStdin = false,
                .externalCommand = NULL,
                .postExternalCommand = NULL,
                .feedbackMutateCommand = NULL,
                .persistent = false,
                .netDriver = false,
                .asLimit = 0U,
                .rssLimit = 0U,
                .dataLimit = 0U,
                .clearEnv = false,
                .env_ptrs = {},
                .env_vals = {},
            },
        .timing =
            {
                .timeStart = time(NULL),
                .runEndTime = 0,
                .tmOut = 10,
                .lastCovUpdate = time(NULL),
                .timeOfLongestUnitInMilliseconds = 0,
                .tmoutVTALRM = false,
            },
        .mutate =
            {
                .mutationsMax = 0,
                .dictionaryFile = NULL,
                .dictionaryCnt = 0,
                .mutationsPerRun = 6U,
                .maxFileSz = 0UL,
            },
        .display =
            {
                .useScreen = true,
                .lastDisplayMillis = util_timeNowMillis(),
                .cmdline_txt[0] = '\0',
            },
        .cfg =
            {
                .useVerifier = false,
                .exitUponCrash = false,
                .report_mutex = PTHREAD_MUTEX_INITIALIZER,
                .reportFile = NULL,
                .dynFileIterExpire = 0,
                .only_printable = false,
                .minimize = false,
                .switchingToFDM = false,
            },
        .sanitizer =
            {
                .enable = false,
                .del_report = false,
            },
        .feedback =
            {
                .feedbackMap = NULL,
                .feedback_mutex = PTHREAD_MUTEX_INITIALIZER,
                .bbFd = -1,
                .blacklistFile = NULL,
                .blacklist = NULL,
                .blacklistCnt = 0,
                .skipFeedbackOnTimeout = false,
                .dynFileMethod = _HF_DYNFILE_SOFT,
                .state = _HF_STATE_UNSET,
            },
        .cnts =
            {
                .mutationsCnt = 0,
                .crashesCnt = 0,
                .uniqueCrashesCnt = 0,
                .verifiedCrashesCnt = 0,
                .blCrashesCnt = 0,
                .timeoutedCnt = 0,
            },
        .socketFuzzer =
            {
                .enabled = false,
                .serverSocket = -1,
                .clientSocket = -1,
            },

        /* Linux code */
        .linux =
            {
                .exeFd = -1,
                .hwCnts =
                    {
                        .cpuInstrCnt = 0ULL,
                        .cpuBranchCnt = 0ULL,
                        .bbCnt = 0ULL,
                        .newBBCnt = 0ULL,
                        .softCntPc = 0ULL,
                        .softCntCmp = 0ULL,
                    },
                .dynamicCutOffAddr = ~(0ULL),
                .disableRandomization = true,
                .ignoreAddr = NULL,
                .symsBlFile = NULL,
                .symsBlCnt = 0,
                .symsBl = NULL,
                .symsWlFile = NULL,
                .symsWlCnt = 0,
                .symsWl = NULL,
                .cloneFlags = 0,
                .kernelOnly = false,
                .useClone = true,
            },
        /* NetBSD code */
        .netbsd =
            {
                .ignoreAddr = NULL,
                .symsBlFile = NULL,
                .symsBlCnt = 0,
                .symsBl = NULL,
                .symsWlFile = NULL,
                .symsWlCnt = 0,
                .symsWl = NULL,
            },
    };

    TAILQ_INIT(&hfuzz->io.dynfileq);
    TAILQ_INIT(&hfuzz->mutate.dictq);

    // clang-format off
    struct custom_option custom_opts[] = {
        { { "help", no_argument, NULL, 'h' }, "Help plz.." },
        { { "input", required_argument, NULL, 'i' }, "Path to a directory containing initial file corpus" },
        { { "output", required_argument, NULL, 'o' }, "Output data (new dynamic coverage corpus, or the minimized coverage corpus) is written to this directory (default: input directory is re-used)" },
        { { "persistent", no_argument, NULL, 'P' }, "Enable persistent fuzzing (use hfuzz_cc/hfuzz-clang to compile code). This will be auto-detected!!!" },
        { { "instrument", no_argument, NULL, 'z' }, "*DEFAULT-MODE-BY-DEFAULT* Enable compile-time instrumentation (use hfuzz_cc/hfuzz-clang to compile code)" },
        { { "minimize", no_argument, NULL, 'M' }, "Minimize the input corpus. It will most likely delete some corpus files (from the --input directory) if no --output is used!" },
        { { "noinst", no_argument, NULL, 'x' }, "Static mode only, disable any instrumentation (hw/sw) feedback" },
        { { "keep_output", no_argument, NULL, 'Q' }, "Don't close children's stdin, stdout, stderr; can be noisy" },
        { { "timeout", required_argument, NULL, 't' }, "Timeout in seconds (default: 10)" },
        { { "threads", required_argument, NULL, 'n' }, "Number of concurrent fuzzing threads (default: number of CPUs / 2)" },
        { { "stdin_input", no_argument, NULL, 's' }, "Provide fuzzing input on STDIN, instead of " _HF_FILE_PLACEHOLDER },
        { { "mutations_per_run", required_argument, NULL, 'r' }, "Maximal number of mutations per one run (default: 6)" },
        { { "logfile", required_argument, NULL, 'l' }, "Log file" },
        { { "verbose", no_argument, NULL, 'v' }, "Disable ANSI console; use simple log output" },
        { { "verifier", no_argument, NULL, 'V' }, "Enable crashes verifier" },
        { { "debug", no_argument, NULL, 'd' }, "Show debug messages (level >= 4)" },
        { { "quiet", no_argument, NULL, 'q' }, "Show only warnings and more serious messages (level <= 1)" },
        { { "extension", required_argument, NULL, 'e' }, "Input file extension (e.g. 'swf'), (default: 'fuzz')" },
        { { "workspace", required_argument, NULL, 'W' }, "Workspace directory to save crashes & runtime files (default: '.')" },
        { { "crashdir", required_argument, NULL, 0x600 }, "Directory where crashes are saved to (default: workspace directory)" },
        { { "covdir_all", required_argument, NULL, 'o' }, "** DEPRECATED ** use --output" },
        { { "covdir_new", required_argument, NULL, 0x602 }, "New coverage (beyond the dry-run fuzzing phase) is written to this separate directory" },
        { { "dict", required_argument, NULL, 'w' }, "Dictionary file. Format:http://llvm.org/docs/LibFuzzer.html#dictionaries" },
        { { "stackhash_bl", required_argument, NULL, 'B' }, "Stackhashes blacklist file (one entry per line)" },
        { { "mutate_cmd", required_argument, NULL, 'c' }, "External command producing fuzz files (instead of internal mutators)" },
        { { "pprocess_cmd", required_argument, NULL, 0x104 }, "External command postprocessing files produced by internal mutators" },
        { { "ffmutate_cmd", required_argument, NULL, 0x110 }, "External command mutating files which have effective coverage feedback" },
        { { "run_time", required_argument, NULL, 0x109 }, "Number of seconds this fuzzing session will last (default: 0 [no limit])" },
        { { "iterations", required_argument, NULL, 'N' }, "Number of fuzzing iterations (default: 0 [no limit])" },
        { { "rlimit_as", required_argument, NULL, 0x100 }, "Per process RLIMIT_AS in MiB (default: 0 [no limit])" },
        { { "rlimit_rss", required_argument, NULL, 0x101 }, "Per process RLIMIT_RSS in MiB (default: 0 [no limit]). It will also set *SAN's soft_rss_limit_mb if used" },
        { { "rlimit_data", required_argument, NULL, 0x102 }, "Per process RLIMIT_DATA in MiB (default: 0 [no limit])" },
        { { "rlimit_core", required_argument, NULL, 0x103 }, "Per process RLIMIT_CORE in MiB (default: 0 [no cores are produced])" },
        { { "report", required_argument, NULL, 'R' }, "Write report to this file (default: '<workdir>/" _HF_REPORT_FILE "')" },
        { { "max_file_size", required_argument, NULL, 'F' }, "Maximal size of files processed by the fuzzer in bytes (default: 134217728 = 128MB)" },
        { { "clear_env", no_argument, NULL, 0x108 }, "Clear all environment variables before executing the binary" },
        { { "env", required_argument, NULL, 'E' }, "Pass this environment variable, can be used multiple times" },
        { { "save_all", no_argument, NULL, 'u' }, "Save all test-cases (not only the unique ones) by appending the current time-stamp to the filenames" },
        { { "tmout_sigvtalrm", no_argument, NULL, 'T' }, "Use SIGVTALRM to kill timeouting processes (default: use SIGKILL)" },
        { { "sanitizers", no_argument, NULL, 'S' }, "** DEPRECATED ** Enable sanitizers settings (default: false)" },
        { { "sanitizers_del_report", required_argument, NULL, 0x10F }, "Delete sanitizer report after use (default: false)" },
        { { "monitor_sigabrt", required_argument, NULL, 0x105 }, "** DEPRECATED ** SIGABRT is always monitored" },
        { { "no_fb_timeout", required_argument, NULL, 0x106 }, "Skip feedback if the process has timeouted (default: false)" },
        { { "exit_upon_crash", no_argument, NULL, 0x107 }, "Exit upon seeing the first crash (default: false)" },
        { { "socket_fuzzer", no_argument, NULL, 0x10B }, "Instrument external fuzzer via socket" },
        { { "netdriver", no_argument, NULL, 0x10C }, "Use netdriver (libhfnetdriver/). In most cases it will be autodetected through a binary signature" },
        { { "only_printable", no_argument, NULL, 0x10D }, "Only generate printable inputs" },
        { { "export_feedback", no_argument, NULL, 0x10E }, "Export the coverage feedback structure as ./hfuzz-feedback" },

#if defined(_HF_ARCH_LINUX)
        { { "linux_symbols_bl", required_argument, NULL, 0x504 }, "Symbols blacklist filter file (one entry per line)" },
        { { "linux_symbols_wl", required_argument, NULL, 0x505 }, "Symbols whitelist filter file (one entry per line)" },
        { { "linux_addr_low_limit", required_argument, NULL, 0x500 }, "Address limit (from si.si_addr) below which crashes are not reported, (default: 0)" },
        { { "linux_keep_aslr", no_argument, NULL, 0x501 }, "Don't disable ASLR randomization, might be useful with MSAN" },
        { { "linux_perf_ignore_above", required_argument, NULL, 0x503 }, "Ignore perf events which report IPs above this address" },
        { { "linux_perf_instr", no_argument, NULL, 0x510 }, "Use PERF_COUNT_HW_INSTRUCTIONS perf" },
        { { "linux_perf_branch", no_argument, NULL, 0x511 }, "Use PERF_COUNT_HW_BRANCH_INSTRUCTIONS perf" },
        { { "linux_perf_bts_edge", no_argument, NULL, 0x513 }, "Use Intel BTS to count unique edges" },
        { { "linux_perf_ipt_block", no_argument, NULL, 0x514 }, "Use Intel Processor Trace to count unique blocks (requires libipt.so)" },
        { { "linux_perf_kernel_only", no_argument, NULL, 0x515 }, "Gather kernel-only coverage with Intel PT and with Intel BTS" },
        { { "linux_ns_net", no_argument, NULL, 0x0530 }, "Use Linux NET namespace isolation" },
        { { "linux_ns_pid", no_argument, NULL, 0x0531 }, "Use Linux PID namespace isolation" },
        { { "linux_ns_ipc", no_argument, NULL, 0x0532 }, "Use Linux IPC namespace isolation" },
#endif // defined(_HF_ARCH_LINUX)

#if defined(_HF_ARCH_NETBSD)
        { { "netbsd_symbols_bl", required_argument, NULL, 0x504 }, "Symbols blacklist filter file (one entry per line)" },
        { { "netbsd_symbols_wl", required_argument, NULL, 0x505 }, "Symbols whitelist filter file (one entry per line)" },
        { { "netbsd_addr_low_limit", required_argument, NULL, 0x500 }, "Address limit (from si.si_addr) below which crashes are not reported, (default: 0)" },
#endif // defined(_HF_ARCH_NETBSD)
        { { 0, 0, 0, 0 }, NULL },
    };
    // clang-format on

    struct option opts[ARRAYSIZE(custom_opts)];
    for (size_t i = 0; i < ARRAYSIZE(custom_opts); i++) {
        opts[i] = custom_opts[i].opt;
    }

    enum llevel_t ll = INFO;
    const char* logfile = NULL;
    int opt_index = 0;
    for (;;) {
        int c = getopt_long(
            argc, argv, "-?hQvVsuPxf:i:dqe:W:r:c:F:t:R:n:N:l:p:g:E:w:B:zMTS", opts, &opt_index);
        if (c < 0) {
            break;
        }

        switch (c) {
            case 'h':
            case '?':
                cmdlineUsage(argv[0], custom_opts);
                break;
            case 'i':
            case 'f': /* Synonym for -i, stands for -f(iles) */
                hfuzz->io.inputDir = optarg;
                break;
            case 'x':
                hfuzz->feedback.dynFileMethod = _HF_DYNFILE_NONE;
                break;
            case 'Q':
                hfuzz->exe.nullifyStdio = false;
                break;
            case 'v':
                hfuzz->display.useScreen = false;
                break;
            case 'V':
                hfuzz->cfg.useVerifier = true;
                break;
            case 's':
                hfuzz->exe.fuzzStdin = true;
                break;
            case 'u':
                hfuzz->io.saveUnique = false;
                break;
            case 'l':
                logfile = optarg;
                break;
            case 'd':
                ll = DEBUG;
                break;
            case 'q':
                ll = WARNING;
                break;
            case 'e':
                hfuzz->io.fileExtn = optarg;
                break;
            case 'W':
                snprintf(hfuzz->io.workDir, sizeof(hfuzz->io.workDir), "%s", optarg);
                break;
            case 0x600:
                hfuzz->io.crashDir = optarg;
                break;
            case 'o':
                hfuzz->io.outputDir = optarg;
                break;
            case 0x602:
                hfuzz->io.covDirNew = optarg;
                break;
            case 'r':
                hfuzz->mutate.mutationsPerRun = strtoul(optarg, NULL, 10);
                break;
            case 'c':
                hfuzz->exe.externalCommand = optarg;
                break;
            case 'S':
                hfuzz->sanitizer.enable = true;
                break;
            case 0x10F:
                hfuzz->sanitizer.del_report = cmdlineParseTrueFalse(opts[opt_index].name, optarg);
                break;
            case 0x10B:
                hfuzz->socketFuzzer.enabled = true;
                hfuzz->timing.tmOut = 0; /* Disable process timeout checks */
                break;
            case 0x10C:
                hfuzz->exe.netDriver = true;
                break;
            case 0x10D:
                hfuzz->cfg.only_printable = true;
                break;
            case 0x10E:
                hfuzz->io.exportFeedback = true;
                break;
            case 'z':
                hfuzz->feedback.dynFileMethod |= _HF_DYNFILE_SOFT;
                break;
            case 'M':
                hfuzz->cfg.minimize = true;
                break;
            case 'F':
                hfuzz->mutate.maxFileSz = strtoul(optarg, NULL, 0);
                break;
            case 't':
                hfuzz->timing.tmOut = atol(optarg);
                break;
            case 'R':
                hfuzz->cfg.reportFile = optarg;
                break;
            case 'n':
                if (optarg[0] == 'a') {
                    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
                    hfuzz->threads.threadsMax = (ncpus < 1 ? 1 : ncpus);
                } else {
                    if (!util_isANumber(optarg)) {
                        LOG_E("'-n %s' is not a number", optarg);
                        return false;
                    }
                    hfuzz->threads.threadsMax = atol(optarg);
                }
                break;
            case 0x109: {
                time_t p = atol(optarg);
                if (p > 0) {
                    hfuzz->timing.runEndTime = time(NULL) + p;
                }
            } break;
            case 'N':
                hfuzz->mutate.mutationsMax = atol(optarg);
                break;
            case 0x100:
                hfuzz->exe.asLimit = strtoull(optarg, NULL, 0);
                break;
            case 0x101:
                hfuzz->exe.rssLimit = strtoull(optarg, NULL, 0);
                break;
            case 0x102:
                hfuzz->exe.dataLimit = strtoull(optarg, NULL, 0);
                break;
            case 0x103:
                hfuzz->exe.coreLimit = strtoull(optarg, NULL, 0);
                break;
            case 0x104:
                hfuzz->exe.postExternalCommand = optarg;
                break;
            case 0x110:
                hfuzz->exe.feedbackMutateCommand = optarg;
                break;
            case 0x106:
                hfuzz->feedback.skipFeedbackOnTimeout = true;
                break;
            case 0x107:
                hfuzz->cfg.exitUponCrash = true;
                break;
            case 0x108:
                hfuzz->exe.clearEnv = true;
                break;
            case 'P':
                hfuzz->exe.persistent = true;
                break;
            case 'T':
                hfuzz->timing.tmoutVTALRM = true;
                break;
            case 'E':
                if (!cmdlineAddEnv(hfuzz, optarg)) {
                    return false;
                }
                break;
            case 'w':
                hfuzz->mutate.dictionaryFile = optarg;
                break;
            case 'B':
                hfuzz->feedback.blacklistFile = optarg;
                break;
#if defined(_HF_ARCH_LINUX)
            case 0x500:
                hfuzz->linux.ignoreAddr = (void*)strtoul(optarg, NULL, 0);
                break;
            case 0x501:
                hfuzz->linux.disableRandomization = false;
                break;
            case 0x503:
                hfuzz->linux.dynamicCutOffAddr = strtoull(optarg, NULL, 0);
                break;
            case 0x504:
                hfuzz->linux.symsBlFile = optarg;
                break;
            case 0x505:
                hfuzz->linux.symsWlFile = optarg;
                break;
            case 0x510:
                hfuzz->feedback.dynFileMethod |= _HF_DYNFILE_INSTR_COUNT;
                break;
            case 0x511:
                hfuzz->feedback.dynFileMethod |= _HF_DYNFILE_BRANCH_COUNT;
                break;
            case 0x513:
                hfuzz->feedback.dynFileMethod |= _HF_DYNFILE_BTS_EDGE;
                break;
            case 0x514:
                hfuzz->feedback.dynFileMethod |= _HF_DYNFILE_IPT_BLOCK;
                break;
            case 0x515:
                hfuzz->linux.kernelOnly = true;
                break;
            case 0x530:
                hfuzz->linux.cloneFlags |= (CLONE_NEWUSER | CLONE_NEWNET);
                break;
            case 0x531:
                hfuzz->linux.cloneFlags |= (CLONE_NEWUSER | CLONE_NEWPID);
                break;
            case 0x532:
                hfuzz->linux.cloneFlags |= (CLONE_NEWUSER | CLONE_NEWIPC);
                break;
#endif /* defined(_HF_ARCH_LINUX) */
#if defined(_HF_ARCH_NETBSD)
            case 0x500:
                hfuzz->netbsd.ignoreAddr = (void*)strtoul(optarg, NULL, 0);
                break;
            case 0x504:
                hfuzz->netbsd.symsBlFile = optarg;
                break;
            case 0x505:
                hfuzz->netbsd.symsWlFile = optarg;
                break;
#endif /* defined(_HF_ARCH_NETBSD) */
            default:
                cmdlineUsage(argv[0], custom_opts);
                return false;
                break;
        }
    }

    logInitLogFile(logfile, -1, ll);

    hfuzz->exe.argc = argc - optind;
    hfuzz->exe.cmdline = (const char* const*)&argv[optind];
    if (hfuzz->exe.argc <= 0) {
        LOG_E("No fuzz command provided");
        cmdlineUsage(argv[0], custom_opts);
        return false;
    }
    if (!files_exists(hfuzz->exe.cmdline[0])) {
        LOG_E("Your fuzzed binary '%s' doesn't seem to exist", hfuzz->exe.cmdline[0]);
        return false;
    }
    if (!cmdlineVerify(hfuzz)) {
        return false;
    }

    display_createTargetStr(hfuzz);

    LOG_I("cmdline:'%s', bin:'%s' inputDir:'%s', fuzzStdin:%s, mutationsPerRun:%u, "
          "timeout:%ld, mutationsMax:%zu, threadsMax:%zu",
        hfuzz->display.cmdline_txt, hfuzz->exe.cmdline[0], hfuzz->io.inputDir,
        cmdlineYesNo(hfuzz->exe.fuzzStdin), hfuzz->mutate.mutationsPerRun,
        (long)hfuzz->timing.tmOut, hfuzz->mutate.mutationsMax, hfuzz->threads.threadsMax);

    return true;
}
