// Crash reporter, after Pikmin 1's pc_crash.cpp. On a fatal signal the binary
// prints the signal, fault address, faulting instruction and a symbolised
// backtrace, plus the input tick when record/replay is linked (replay with
// P2_EXIT_TICK just before it to stop at the crash). It then re-raises so the
// OS still writes its crash report. Everything is async-signal-safe: no
// allocation, fixed buffers, backtrace_symbols_fd.

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <stdint.h>

extern "C" uint32_t p2_input_current_tick() __attribute__((weak));

namespace {

void write_str(const char* s) { (void)!write(STDERR_FILENO, s, strlen(s)); }

// Minimal unsigned-hex writer — snprintf is not async-signal-safe.
void write_hex(unsigned long long v)
{
	char buf[19];
	int i    = sizeof(buf) - 1;
	buf[i--] = '\0';
	if (v == 0) {
		buf[i--] = '0';
	}
	while (v && i >= 2) {
		const char* d = "0123456789abcdef";
		buf[i--]      = d[v & 0xF];
		v >>= 4;
	}
	buf[i--] = 'x';
	buf[i]   = '0';
	write_str(&buf[i]);
}

void write_dec(unsigned long long v)
{
	char buf[21];
	int i    = sizeof(buf) - 1;
	buf[i]   = '\0';
	do {
		buf[--i] = char('0' + v % 10);
		v /= 10;
	} while (v && i > 0);
	write_str(&buf[i]);
}

const char* signal_name(int sig)
{
	switch (sig) {
	case SIGSEGV: return "SIGSEGV (bad memory access)";
	case SIGBUS:  return "SIGBUS (bad address / write to read-only memory)";
	case SIGILL:  return "SIGILL (illegal instruction)";
	case SIGFPE:  return "SIGFPE (arithmetic fault)";
	case SIGABRT: return "SIGABRT (abort)";
	case SIGTRAP: return "SIGTRAP (trap)";
	default:      return "signal";
	}
}

void handler(int sig, siginfo_t* info, void* uap)
{
	write_str("\n================ [P2] CRASH ================\n");
	write_str("signal: ");
	write_str(signal_name(sig));
	if (p2_input_current_tick) {
		write_str("\ninput tick: ");
		write_dec(p2_input_current_tick());
	}
	if (info && (sig == SIGSEGV || sig == SIGBUS)) {
		write_str("\nfault address: ");
		write_hex((unsigned long long)(unsigned long)info->si_addr);
	}
#if defined(__arm64__) || defined(__aarch64__)
	// The faulting instruction itself. backtrace() walks frame pointers, so a
	// LEAF function — one that never pushes a frame — does not appear in it,
	// and the innermost frame is exactly the one worth having. Verified: a
	// deliberate null store in a leaf showed the caller as frame 2 with the
	// leaf missing entirely. Taking the PC out of the signal context closes
	// that gap.
	if (uap) {
		const ucontext_t* uc = (const ucontext_t*)uap;
		if (uc->uc_mcontext) {
			// Symbolise it through backtrace_symbols_fd rather than printing
			// raw hex: the raw value needs the ASLR slide before atos can do
			// anything with it, and this prints module + symbol + offset
			// directly.
			void* pc[2];
			pc[0] = (void*)(unsigned long)uc->uc_mcontext->__ss.__pc;
			pc[1] = (void*)(unsigned long)uc->uc_mcontext->__ss.__lr;
			write_str("\nfaulting instruction:\n");
			backtrace_symbols_fd(&pc[0], 1, STDERR_FILENO);
			write_str("called from (link register):\n");
			backtrace_symbols_fd(&pc[1], 1, STDERR_FILENO);
		}
	}
#endif
	write_str("backtrace (frame-pointer walk; a leaf frame may be absent —\n"
	          "           the two lines above are the innermost site):\n");

	void* frames[64];
	int n = backtrace(frames, 64);
	// Writes straight to the fd without allocating.
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	write_str("============================================\n"
	          "Paste the block above. To turn addresses into file:line:\n"
	          "  atos -o <the p2 binary> -l <load address> <address>\n"
	          "============================================\n");

	// Restore the default disposition and re-raise, so the OS still produces
	// its own crash report in ~/Library/Logs/DiagnosticReports.
	signal(sig, SIG_DFL);
	raise(sig);
}

} // namespace

extern "C" void p2_crash_handler_install(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);

	static const int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
	for (unsigned i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); i++) {
		sigaction(kSignals[i], &sa, nullptr);
	}
}
