/**************************************************************************/
/*  crash_handler_windows_seh.cpp                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "crash_handler_windows.h"

#include "core/config/project_settings.h"
#include "core/object/script_language.h"
#include "core/object/worker_thread_pool.h" // LONGSHOT patch #72: the aborting thread named by the pool's index
#include "core/os/main_loop.h"
#include "core/os/os.h"
#include "core/os/thread.h" // LONGSHOT patch #72: the aborting thread named by the engine's own id
#include "core/string/print_string.h"
#include "core/version.h"

#ifdef CRASH_HANDLER_EXCEPTION

// Backtrace code based on: https://stackoverflow.com/questions/6205981/windows-c-stack-trace-from-a-running-app

#include <psapi.h>

#include <algorithm>
#include <atomic> // LONGSHOT patch #72
#include <csignal> // LONGSHOT patch #72
#include <cstdio> // LONGSHOT patch #72
#include <cstdlib>
#include <exception> // LONGSHOT patch #72
#include <iterator>
#include <string>
#include <vector>

// Some versions of imagehlp.dll lack the proper packing directives themselves
// so we need to do it.
#pragma pack(push, before_imagehlp, 8)
#include <imagehlp.h>
#pragma pack(pop, before_imagehlp)

struct module_data {
	std::string image_name;
	std::string module_name;
	void *base_address = nullptr;
	DWORD load_size;
};

class symbol {
	typedef IMAGEHLP_SYMBOL64 sym_type;
	sym_type *sym;
	static const int max_name_len = 1024;

public:
	symbol(HANDLE process, DWORD64 address) :
			sym((sym_type *)::operator new(sizeof(*sym) + max_name_len)) {
		memset(sym, '\0', sizeof(*sym) + max_name_len);
		sym->SizeOfStruct = sizeof(*sym);
		sym->MaxNameLength = max_name_len;
		DWORD64 displacement;

		SymGetSymFromAddr64(process, address, &displacement, sym);
	}

	std::string name() { return std::string(sym->Name); }
	std::string undecorated_name() {
		if (*sym->Name == '\0') {
			return "<couldn't map PC to fn name>";
		}
		std::vector<char> und_name(max_name_len);
		UnDecorateSymbolName(sym->Name, &und_name[0], max_name_len, UNDNAME_COMPLETE);
		return std::string(&und_name[0], strlen(&und_name[0]));
	}
};

class get_mod_info {
	HANDLE process;

public:
	get_mod_info(HANDLE h) :
			process(h) {}

	module_data operator()(HMODULE module) {
		module_data ret;
		char temp[4096];
		MODULEINFO mi;

		GetModuleInformation(process, module, &mi, sizeof(mi));
		ret.base_address = mi.lpBaseOfDll;
		ret.load_size = mi.SizeOfImage;

		GetModuleFileNameEx(process, module, temp, sizeof(temp));
		ret.image_name = temp;
		GetModuleBaseName(process, module, temp, sizeof(temp));
		ret.module_name = temp;
		SymLoadModule64(process, nullptr, ret.image_name.c_str(), ret.module_name.c_str(), (DWORD64)ret.base_address, ret.load_size);
		return ret;
	}
};

DWORD CrashHandlerException(EXCEPTION_POINTERS *ep) {
	HANDLE process = GetCurrentProcess();
	HANDLE hThread = GetCurrentThread();
	DWORD offset_from_symbol = 0;
	IMAGEHLP_LINE64 line = {};
	std::vector<module_data> modules;
	DWORD cbNeeded;
	std::vector<HMODULE> module_handles(1);

	if (OS::get_singleton() == nullptr || OS::get_singleton()->is_disable_crash_handler() || IsDebuggerPresent()) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (OS::get_singleton()->is_crash_handler_silent()) {
		std::_Exit(0);
	}

	String msg;
	if (ProjectSettings::get_singleton()) {
		msg = GLOBAL_GET("debug/settings/crash_handler/message");
	}

	// Tell MainLoop about the crash. This can be handled by users too in Node.
	if (OS::get_singleton()->get_main_loop()) {
		OS::get_singleton()->get_main_loop()->notification(MainLoop::NOTIFICATION_CRASH);
	}

	print_error("\n================================================================");
	print_error(vformat("%s: Program crashed", __FUNCTION__));

	// Print the engine version just before, so that people are reminded to include the version in backtrace reports.
	if (String(GODOT_VERSION_HASH).is_empty()) {
		print_error(vformat("Engine version: %s", GODOT_VERSION_FULL_NAME));
	} else {
		print_error(vformat("Engine version: %s (%s)", GODOT_VERSION_FULL_NAME, GODOT_VERSION_HASH));
	}
	print_error(vformat("Dumping the backtrace. %s", msg));

	// Load the symbols:
	// LONGSHOT patch #72: a process whose symbols were initialised before (a backtrace printed earlier, a stack read) refuses a second
	// SymInitialize and the handler left without a frame ("Dumping the backtrace" and nothing after it); cleaned up and asked again
	if (!SymInitialize(process, nullptr, false)) {
		SymCleanup(process);
		if (!SymInitialize(process, nullptr, false)) {
			print_error(vformat("LONGSHOT: the symbols did not initialise (error %d): no frames", (int)GetLastError()));
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}
	print_error("LONGSHOT: the symbols initialised; the modules next (the loader lock), then the walk");

	SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_EXACT_SYMBOLS);
	EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
	module_handles.resize(cbNeeded / sizeof(HMODULE));
	EnumProcessModules(process, &module_handles[0], module_handles.size() * sizeof(HMODULE), &cbNeeded);
	std::transform(module_handles.begin(), module_handles.end(), std::back_inserter(modules), get_mod_info(process));
	void *base = modules[0].base_address;

	print_error(vformat("Load address: %x (%d modules)\n", (uint64_t)base, (int)modules.size()));

	// Setup stuff:
	CONTEXT *context = ep->ContextRecord;
	STACKFRAME64 frame;
	bool skip_first = false;

	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;

#if defined(_M_X64)
	frame.AddrPC.Offset = context->Rip;
	frame.AddrStack.Offset = context->Rsp;
	frame.AddrFrame.Offset = context->Rbp;
#elif defined(_M_ARM64) || defined(_M_ARM64EC)
	frame.AddrPC.Offset = context->Pc;
	frame.AddrStack.Offset = context->Sp;
	frame.AddrFrame.Offset = context->Fp;
#elif defined(_M_ARM)
	frame.AddrPC.Offset = context->Pc;
	frame.AddrStack.Offset = context->Sp;
	frame.AddrFrame.Offset = context->R11;
#else
	frame.AddrPC.Offset = context->Eip;
	frame.AddrStack.Offset = context->Esp;
	frame.AddrFrame.Offset = context->Ebp;

	// Skip the first one to avoid a duplicate on 32-bit mode
	skip_first = true;
#endif

	line.SizeOfStruct = sizeof(line);
	IMAGE_NT_HEADERS *h = ImageNtHeader(base);
	DWORD image_type = h->FileHeader.Machine;

	int n = 0;
	do {
		if (skip_first) {
			skip_first = false;
		} else {
			if (frame.AddrPC.Offset != 0) {
				std::string fnName = symbol(process, frame.AddrPC.Offset).undecorated_name();

				IMAGEHLP_MODULE64 mod_info;
				memset(&mod_info, 0, sizeof(IMAGEHLP_MODULE64));
				mod_info.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
				uint64_t offset = (uint64_t)base;
				String mod_name = "main";
				if (SymGetModuleInfo64(process, frame.AddrPC.Offset, &mod_info)) {
					offset = mod_info.BaseOfImage;
					if (offset != (uint64_t)base) {
						if (mod_info.ImageName[0] != 0) {
							mod_name = String((const char *)mod_info.ImageName).to_lower().get_file();
						} else if (mod_info.ModuleName[0] != 0) {
							mod_name = String((const char *)mod_info.ModuleName).to_lower();
						} else {
							mod_name = "<unknown module>";
						}
					}
				}
				if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &offset_from_symbol, &line)) {
					print_error(vformat("[%d] %x (%s+%x) - %s (%s:%d)", n, (uint64_t)frame.AddrPC.Offset, mod_name, (uint64_t)frame.AddrPC.Offset - offset, fnName.c_str(), (char *)line.FileName, (int)line.LineNumber));
				} else if (!fnName.empty()) {
					print_error(vformat("[%d] %x (%s+%x) - %s", n, (uint64_t)frame.AddrPC.Offset, mod_name, (uint64_t)frame.AddrPC.Offset - offset, fnName.c_str()));
				} else {
					print_error(vformat("[%d] %x (%s+%x) - ???", n, (uint64_t)frame.AddrPC.Offset, mod_name, (uint64_t)frame.AddrPC.Offset - offset));
				}
			}

			n++;
		}

		if (!StackWalk64(image_type, process, hThread, &frame, context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
			break;
		}
	} while (frame.AddrReturn.Offset != 0 && n < 256);

	print_error("-- END OF C++ BACKTRACE --");
	print_error("================================================================");

	SymCleanup(process);

	for (const Ref<ScriptBacktrace> &backtrace : ScriptServer::capture_script_backtraces(false)) {
		if (!backtrace->is_empty()) {
			print_error(backtrace->format());
			print_error(vformat("-- END OF %s BACKTRACE --", backtrace->get_language_name().to_upper()));
			print_error("================================================================");
		}
	}

	// Pass the exception to the OS
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

CrashHandler::CrashHandler() {
	disabled = false;
}

CrashHandler::~CrashHandler() {
}

void CrashHandler::disable() {
	if (disabled) {
		return;
	}

	disabled = true;
}

#ifdef CRASH_HANDLER_EXCEPTION
// LONGSHOT patch #72 (THE ABORT NAMED): an abort - the C runtime's own, or an uncaught throw's std::terminate - leaves the process by a
// fast-fail (0xC0000409) that no exception filter sees: no backtrace, no fault record (the lane's smokes returned 127 at a clean exit
// for an hour before the raw code said what it was). The abort's signal and the terminate handler capture the thread's context and print
// the same backtrace as a crash, then let the abort go on as it would
// THE FIRST ABORT WALKS, A SECOND HOLDS (10-27 02:1x): the C runtime arms the abort's signal once - the first abort's handler runs, and a
// second abort on any thread meets the default and ends the process by its fast-fail while the first still loads the symbols (the
// instruments smoke, 01:52: the header, no frame, the process gone from inside the main thread's cleanup). The handler arms the signal
// again as it starts; the first aborting thread walks, a later one says its own raw frames and holds until the walker's return ends the
// process. THE RAW FRAMES FIRST: before the symbols (dbghelp, the loader lock) a thread says its return addresses as offsets from the
// executable's base, lock-free and straight to stderr, so a walk that never returns still leaves the trace the build's symbols resolve
static std::atomic<DWORD> g_abort_walker{ 0 };

static void _longshot_say(const char *p_line) {
	fputs(p_line, stderr);
	fputc('\n', stderr);
	fflush(stderr);
}

static void _longshot_raw_frames(const char *p_what, DWORD p_thread) {
	void *frames[62];
	const USHORT n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
	const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
	char line[4096];
	int w = snprintf(line, sizeof(line), "LONGSHOT: %s on thread %lu - its %d frames as offsets from the executable's base (another module's as its address):", p_what, (unsigned long)p_thread, (int)n);
	for (USHORT i = 0; i < n && w > 0 && w < int(sizeof(line)) - 40; i++) {
		const uintptr_t a = (uintptr_t)frames[i];
		if (a >= base && a - base < (uintptr_t)0x40000000) {
			w += snprintf(line + w, sizeof(line) - size_t(w), " +%llx", (unsigned long long)(a - base));
		} else {
			w += snprintf(line + w, sizeof(line) - size_t(w), " %llx", (unsigned long long)a);
		}
	}
	_longshot_say(line);
}

static void _longshot_abort_backtrace(const char *p_what) {
	const DWORD me = GetCurrentThreadId();
	DWORD walker = 0;
	if (!g_abort_walker.compare_exchange_strong(walker, me)) {
		if (walker == me) {
			_longshot_say("LONGSHOT: the walk itself aborted - the process ends as the abort would");
			return;
		}
		_longshot_raw_frames(p_what, me);
		char line[256];
		snprintf(line, sizeof(line), "LONGSHOT: thread %lu holds while thread %lu walks its backtrace", (unsigned long)me, (unsigned long)walker);
		_longshot_say(line);
		Sleep(INFINITE);
	}
	_longshot_raw_frames(p_what, me);
	CONTEXT ctx;
	RtlCaptureContext(&ctx);
	EXCEPTION_RECORD rec = {};
	rec.ExceptionCode = 0xC0000409; // what the abort returns to the shell
	EXCEPTION_POINTERS ep = { &rec, &ctx };
	// the aborting thread named: the engine's own id (a small ordinal for a thread the engine started, a hash for one made outside it), the
	// pool's index when it is a worker, its description (the engine names its threads) and its os id
	String who = Thread::is_main_thread() ? String("the main thread") : vformat("thread %d", (int64_t)Thread::get_caller_id());
	if (!Thread::is_main_thread() && WorkerThreadPool::get_singleton() && WorkerThreadPool::get_singleton()->get_thread_index() >= 0) {
		who += vformat(" (worker %d of the pool)", WorkerThreadPool::get_singleton()->get_thread_index());
	}
	String thread_name;
	{
		PWSTR desc = nullptr;
		if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) {
			thread_name = String::utf16((const char16_t *)desc);
			LocalFree(desc);
		}
	}
	print_error(vformat("LONGSHOT: %s ends the process on %s ('%s', os id %d) - the backtrace is that thread's at the abort", p_what, who, thread_name, (int)me));
	CrashHandlerException(&ep);
}

static void _longshot_sigabrt(int) {
	signal(SIGABRT, _longshot_sigabrt); // the runtime arms it once: armed again, so a second thread's abort is told and held, not the process' end
	_longshot_abort_backtrace("an abort");
}

static void _longshot_terminate() {
	// (the runtime keeps a terminate handler per thread: this one serves the main thread; another thread's terminate aborts, and the
	// abort's signal, process-wide, tells it)
	_longshot_abort_backtrace("an uncaught exception (std::terminate)");
	signal(SIGABRT, SIG_DFL);
	abort();
}
#endif

void CrashHandler::initialize() {
#ifdef CRASH_HANDLER_EXCEPTION
	signal(SIGABRT, _longshot_sigabrt); // LONGSHOT patch #72
	std::set_terminate(_longshot_terminate);
#endif
}
